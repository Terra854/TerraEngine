// RagdollProfileValidator.cpp - Deterministic cross-asset ragdoll validation.

#include "Animation/Ragdoll/RagdollProfileValidator.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <tuple>

namespace TerraEngine::Animation {
	namespace {
		inline constexpr std::size_t kMaximumDiagnostics = 1024U;
		inline constexpr float kVectorTolerance = 0.001F;

		float Dot(const AnimationFloat3& left, const AnimationFloat3& right) noexcept {
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		bool IsUnitVector(const AnimationFloat3& value) noexcept {
			return IsFinite(value) && std::abs(Dot(value, value) - 1.0F) <= kVectorTolerance;
		}

		bool IsValidFrame(const RagdollConstraintFrame& frame) noexcept {
			return IsFinite(frame.localPosition) && IsNormalized(frame.localOrientation)
				&& IsUnitVector(frame.primaryAxis) && IsUnitVector(frame.secondaryAxis)
				&& std::abs(Dot(frame.primaryAxis, frame.secondaryAxis)) <= kVectorTolerance;
		}

		bool IsValidLimit(const RagdollAxisLimit& limit) noexcept {
			const bool supported = limit.mode >= RagdollLimitMode::Free
				&& limit.mode <= RagdollLimitMode::Locked;
			if(!supported || !std::isfinite(limit.minimum) || !std::isfinite(limit.maximum)
				|| !std::isfinite(limit.restitution) || limit.restitution < 0.0F
				|| limit.restitution > 1.0F || !std::isfinite(limit.springFrequencyHertz)
				|| limit.springFrequencyHertz < 0.0F
				|| !std::isfinite(limit.springDampingRatio)
				|| limit.springDampingRatio < 0.0F) {
				return false;
			}
			if(limit.mode == RagdollLimitMode::Limited) {
				return limit.minimum <= limit.maximum;
			}
			return limit.mode != RagdollLimitMode::Locked
				|| std::abs(limit.minimum - limit.maximum) <= 0.000001F;
		}

		bool IsValidMotor(const RagdollAxisMotor& motor) noexcept {
			return motor.mode >= RagdollMotorMode::Disabled
				&& motor.mode <= RagdollMotorMode::Velocity
				&& std::isfinite(motor.targetPosition)
				&& std::isfinite(motor.targetVelocity)
				&& std::isfinite(motor.stiffness) && motor.stiffness >= 0.0F
				&& std::isfinite(motor.damping) && motor.damping >= 0.0F
				&& std::isfinite(motor.maximumForce) && motor.maximumForce >= 0.0F;
		}

		bool IsValidRecoveryMetadata(const RagdollRecoveryRecord& recovery) noexcept {
			return recovery.candidateId != 0U && recovery.clipAssetId.IsValid()
				&& recovery.clipGeneration != 0U && recovery.clipSkeletonFingerprint != 0U
				&& recovery.requiredReferencePoseHash != 0U
				&& recovery.rootJointStableId != 0U && IsUnitVector(recovery.facing)
				&& std::isfinite(recovery.minimumUprightDot)
				&& recovery.minimumUprightDot >= -1.0F && recovery.minimumUprightDot <= 1.0F
				&& recovery.posture >= RagdollRecoveryPosture::Any
				&& recovery.posture <= RagdollRecoveryPosture::RightSide
				&& IsFinite(recovery.rootAlignmentOffset)
				&& IsNormalized(recovery.rootAlignmentOrientation)
				&& std::isfinite(recovery.maximumPoseErrorMeters)
				&& recovery.maximumPoseErrorMeters >= 0.0F
				&& std::isfinite(recovery.maximumLinearSpeed)
				&& recovery.maximumLinearSpeed >= 0.0F
				&& std::isfinite(recovery.maximumAngularSpeed)
				&& recovery.maximumAngularSpeed >= 0.0F
				&& std::isfinite(recovery.blendDurationSeconds)
				&& recovery.blendDurationSeconds >= 0.0F
				&& std::isfinite(recovery.clearanceRadiusMeters)
				&& recovery.clearanceRadiusMeters > 0.0F
				&& std::isfinite(recovery.clearanceHeightMeters)
				&& recovery.clearanceHeightMeters > 0.0F;
		}

		bool IsValidColliderSummary(const RagdollColliderRecord& collider) noexcept {
			if(collider.colliderId == 0U || collider.shape < RagdollColliderShape::Sphere
				|| collider.shape > RagdollColliderShape::ConvexHull
				|| collider.convexHullSource < RagdollConvexHullSource::None
				|| collider.convexHullSource
					> RagdollConvexHullSource::ImmutableCookedAsset
				|| !IsFinite(collider.localCenter) || !IsNormalized(collider.localOrientation)
				|| !IsFinite(collider.dimensions) || !std::isfinite(collider.friction)
				|| collider.friction < 0.0F || !std::isfinite(collider.restitution)
				|| collider.restitution < 0.0F || collider.restitution > 1.0F) {
				return false;
			}
			if(collider.shape != RagdollColliderShape::ConvexHull) {
				const bool dimensionsValid = collider.shape == RagdollColliderShape::Sphere
					? collider.dimensions.x > 0.0F
					: collider.shape == RagdollColliderShape::Capsule
						? collider.dimensions.x > 0.0F && collider.dimensions.y >= 0.0F
						: collider.dimensions.x > 0.0F && collider.dimensions.y > 0.0F
							&& collider.dimensions.z > 0.0F;
				return dimensionsValid
					&& collider.convexHullSource == RagdollConvexHullSource::None
					&& collider.convexHullVertices.empty()
					&& !collider.immutableShape.assetId.IsValid();
			}
			if(collider.convexHullSource == RagdollConvexHullSource::InlineVertices) {
				return collider.convexHullVertices.size() >= 4U
					&& collider.convexHullVertices.size()
						<= kRagdollProfileMaximumConvexHullVerticesPerCollider
					&& !collider.immutableShape.assetId.IsValid();
			}
			return collider.convexHullSource == RagdollConvexHullSource::ImmutableCookedAsset
				&& collider.convexHullVertices.empty() && collider.immutableShape.assetId.IsValid()
				&& collider.immutableShape.expectedGeneration != 0U
				&& collider.immutableShape.compatibilityFingerprint != 0U;
		}

		class ReportBuilder {
		public:
			explicit ReportBuilder(RagdollProfileValidationReport& report) noexcept
				: report_(report) {}

			void Add(
				RagdollProfileDiagnosticCode code,
				AnimationStatus status,
				std::string_view message,
				std::string_view correctiveAction,
				std::uint64_t bodyId = 0U,
				std::uint64_t constraintId = 0U,
				std::uint64_t stableJointId = 0U,
				std::uint64_t recoveryCandidateId = 0U) {
				hasError_ = true;
				if(report_.diagnostics.size() >= kMaximumDiagnostics) {
					return;
				}
				report_.diagnostics.push_back({
					code,
					status,
					RagdollProfileDiagnosticSeverity::Error,
					bodyId,
					constraintId,
					stableJointId,
					recoveryCandidateId,
					message,
					correctiveAction
				});
			}

			void Finish() {
				std::sort(report_.rootBodyIds.begin(), report_.rootBodyIds.end());
				std::sort(report_.missingRequiredJointIds.begin(),
					report_.missingRequiredJointIds.end());
				report_.missingRequiredJointIds.erase(std::unique(
					report_.missingRequiredJointIds.begin(),
					report_.missingRequiredJointIds.end()),
					report_.missingRequiredJointIds.end());
				std::sort(report_.diagnostics.begin(), report_.diagnostics.end(),
					[](const RagdollProfileDiagnostic& left,
						const RagdollProfileDiagnostic& right) {
						return std::tie(left.code, left.bodyId, left.constraintId,
							left.stableJointId, left.recoveryCandidateId, left.status)
							< std::tie(right.code, right.bodyId, right.constraintId,
								right.stableJointId, right.recoveryCandidateId, right.status);
					});
				if(hasError_) {
					report_.status = report_.diagnostics.empty()
						? AnimationStatus::Rejected : report_.diagnostics.front().status;
				}
			}

		private:
			RagdollProfileValidationReport& report_;
			bool hasError_ = false;
		};

		bool IsAncestor(
			std::size_t parentIndex,
			std::size_t childIndex,
			const SkeletonAsset& skeleton) noexcept {
			std::size_t current = childIndex;
			while(current < skeleton.joints.size()) {
				if(current == parentIndex) {
					return true;
				}
				const std::int32_t parent = skeleton.joints[current].parentIndex;
				if(parent < 0) {
					return false;
				}
				current = static_cast<std::size_t>(parent);
			}
			return false;
		}

		bool VisitConstraintGraph(
			std::uint64_t bodyId,
			const std::map<std::uint64_t, std::vector<std::uint64_t>>& children,
			std::set<std::uint64_t>& visited,
			std::set<std::uint64_t>& active,
			std::uint64_t& cycleBodyId) {
			if(active.contains(bodyId)) {
				cycleBodyId = bodyId;
				return false;
			}
			if(!visited.insert(bodyId).second) {
				return true;
			}
			active.insert(bodyId);
			const auto found = children.find(bodyId);
			if(found != children.end()) {
				for(const std::uint64_t childBodyId : found->second) {
					if(!VisitConstraintGraph(childBodyId, children, visited,
						active, cycleBodyId)) {
						return false;
					}
				}
			}
			active.erase(bodyId);
			return true;
		}
	}

	RagdollProfileValidationReport ValidateRagdollProfile(
		const RagdollProfileAsset& asset,
		const SkeletonAsset& skeleton,
		RagdollProfileValidationOptions options) noexcept {
		try {
			RagdollProfileValidationReport report;
			ReportBuilder builder(report);
			const AnimationStatus structureStatus =
				ValidateRagdollProfileAssetStructure(asset);
			if(structureStatus != AnimationStatus::Ok) {
				builder.Add(RagdollProfileDiagnosticCode::InvalidStructure,
					structureStatus,
					"The ragdoll profile contains an invalid or out-of-bounds record.",
					"Correct the identified body, collider, constraint, blend, or recovery data.");
				if(structureStatus == AnimationStatus::HardLimitExceeded
					|| structureStatus == AnimationStatus::UnsupportedMajorVersion
					|| structureStatus == AnimationStatus::InternalError) {
					builder.Finish();
					return report;
				}
			}

			if(!std::isfinite(options.minimumTotalMassKilograms)
				|| !std::isfinite(options.maximumTotalMassKilograms)
				|| options.minimumTotalMassKilograms < 0.0F
				|| options.minimumTotalMassKilograms > options.maximumTotalMassKilograms) {
				builder.Add(RagdollProfileDiagnosticCode::InvalidStructure,
					AnimationStatus::InvalidArgument,
					"The validation mass range is invalid.",
					"Provide a finite minimum that does not exceed the maximum.");
			}

			if(asset.skeletonAssetId != skeleton.assetId) {
				builder.Add(RagdollProfileDiagnosticCode::SkeletonAssetMismatch,
					AnimationStatus::IncompatibleAsset,
					"The profile names a different skeleton asset.",
					"Rebind or regenerate the profile for this skeleton.");
			}
			if(asset.skeletonFingerprint != skeleton.compatibilityFingerprint) {
				builder.Add(RagdollProfileDiagnosticCode::SkeletonFingerprintMismatch,
					AnimationStatus::IncompatibleAsset,
					"The profile skeleton fingerprint does not match the loaded skeleton.",
					"Revalidate the profile after the skeleton topology change.");
			}
			if(asset.referencePoseHash != skeleton.referencePoseHash) {
				builder.Add(RagdollProfileDiagnosticCode::ReferencePoseMismatch,
					AnimationStatus::IncompatibleAsset,
					"The profile reference pose does not match the skeleton reference pose.",
					"Regenerate body offsets and recovery alignment from the current reference pose.");
			}

			std::map<std::uint64_t, std::size_t> skeletonJointIndices;
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				skeletonJointIndices.emplace(skeleton.joints[index].stableId, index);
			}
			std::map<std::uint64_t, const RagdollBodyRecord*> bodiesById;
			std::map<std::uint64_t, std::uint64_t> bodyByJoint;
			std::set<std::uint64_t> colliderIds;
			double totalMass = 0.0;
			for(const RagdollBodyRecord& body : asset.bodies) {
				if(!bodiesById.emplace(body.bodyId, &body).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateBodyId,
						AnimationStatus::Rejected,
						"Two ragdoll bodies use the same stable body ID.",
						"Assign a unique stable body ID.", body.bodyId);
				}
				if(!bodyByJoint.emplace(body.stableJointId, body.bodyId).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateBodyJoint,
						AnimationStatus::Rejected,
						"More than one body maps to the same skeleton joint.",
						"Keep one body mapping for each stable joint.",
						body.bodyId, 0U, body.stableJointId);
				}
				if(!skeletonJointIndices.contains(body.stableJointId)) {
					builder.Add(RagdollProfileDiagnosticCode::UnknownBodyJoint,
						AnimationStatus::IncompatibleAsset,
						"A ragdoll body maps to a joint that is absent from the skeleton.",
						"Choose a stable joint ID from the current skeleton.",
						body.bodyId, 0U, body.stableJointId);
				}
				if(!std::isfinite(body.massKilograms) || body.massKilograms <= 0.0F) {
					builder.Add(RagdollProfileDiagnosticCode::InvalidBodyMass,
						AnimationStatus::Rejected,
						"A body mass is not finite and positive.",
						"Author a positive mass in kilograms.", body.bodyId);
				} else {
					totalMass += static_cast<double>(body.massKilograms);
				}
				for(const RagdollColliderRecord& collider : body.colliders) {
					if(!colliderIds.insert(collider.colliderId).second) {
						builder.Add(RagdollProfileDiagnosticCode::DuplicateColliderId,
							AnimationStatus::Rejected,
							"Two colliders use the same stable collider ID.",
							"Assign a unique collider ID across the profile.", body.bodyId);
					}
					if(!IsValidColliderSummary(collider)) {
						builder.Add(RagdollProfileDiagnosticCode::InvalidCollider,
							AnimationStatus::Rejected,
							"A collider recipe or convex-hull source is invalid.",
							"Use finite primitive dimensions, inline hull vertices, or a valid immutable shape reference.",
							body.bodyId);
					}
				}
			}
			report.totalMassKilograms = static_cast<float>(totalMass);

			if(!bodiesById.contains(asset.rootBodyId)) {
				builder.Add(RagdollProfileDiagnosticCode::RootBodyMissing,
					AnimationStatus::Rejected,
					"The declared root body is absent from the body table.",
					"Select one mapped body as the root.", asset.rootBodyId);
			}

			std::set<std::uint64_t> requiredJoints;
			if(asset.coveragePolicy == RagdollCoveragePolicy::RequiredJointSet) {
				requiredJoints.insert(asset.requiredBodyJointStableIds.begin(),
					asset.requiredBodyJointStableIds.end());
			} else if(asset.coveragePolicy == RagdollCoveragePolicy::AllDeformingJoints) {
				for(const SkeletonJoint& joint : skeleton.joints) {
					if(joint.deformsMesh) {
						requiredJoints.insert(joint.stableId);
					}
				}
			}
			for(const std::uint64_t jointId : requiredJoints) {
				if(!bodyByJoint.contains(jointId)) {
					report.missingRequiredJointIds.push_back(jointId);
					builder.Add(RagdollProfileDiagnosticCode::MissingRequiredBody,
						AnimationStatus::Rejected,
						"A joint required by the coverage policy has no ragdoll body.",
						"Add a body or relax the explicit coverage policy.",
						0U, 0U, jointId);
				}
			}

			std::map<std::uint64_t, std::uint32_t> inboundCounts;
			std::map<std::uint64_t, std::vector<std::uint64_t>> children;
			for(const auto& [bodyId, body] : bodiesById) {
				static_cast<void>(body);
				inboundCounts.emplace(bodyId, 0U);
			}
			std::set<std::uint64_t> constraintIds;
			std::set<std::pair<std::uint64_t, std::uint64_t>> constraintPairs;
			for(const RagdollConstraintRecord& constraint : asset.constraints) {
				if(!constraintIds.insert(constraint.constraintId).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateConstraintId,
						AnimationStatus::Rejected,
						"Two constraints use the same stable constraint ID.",
						"Assign a unique constraint ID.",
						0U, constraint.constraintId);
				}
				const std::pair<std::uint64_t, std::uint64_t> orderedPair{
					std::min(constraint.parentBodyId, constraint.childBodyId),
					std::max(constraint.parentBodyId, constraint.childBodyId)};
				if(!constraintPairs.insert(orderedPair).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateConstraintPair,
						AnimationStatus::Rejected,
						"More than one constraint connects the same body pair.",
						"Keep one authored constraint for this body pair.",
						0U, constraint.constraintId);
				}
				const auto parent = bodiesById.find(constraint.parentBodyId);
				const auto child = bodiesById.find(constraint.childBodyId);
				if(parent == bodiesById.end() || child == bodiesById.end()) {
					builder.Add(RagdollProfileDiagnosticCode::UnknownConstraintBody,
						AnimationStatus::Rejected,
						"A constraint endpoint does not name an existing body.",
						"Repair the parent and child body IDs.",
						0U, constraint.constraintId);
				} else if(parent->first == child->first) {
					builder.Add(RagdollProfileDiagnosticCode::UnknownConstraintBody,
						AnimationStatus::Rejected,
						"A constraint cannot connect a body to itself.",
						"Choose distinct parent and child bodies.",
						parent->first, constraint.constraintId);
				} else {
					const std::uint32_t inbound = ++inboundCounts[child->first];
					children[parent->first].push_back(child->first);
					if(inbound > 1U) {
						builder.Add(RagdollProfileDiagnosticCode::MultipleParentConstraints,
							AnimationStatus::Rejected,
							"A ragdoll body has more than one parent constraint.",
							"Keep one incoming structural constraint for each non-root body.",
							child->first, constraint.constraintId,
							child->second->stableJointId);
					}
					const auto parentJoint = skeletonJointIndices.find(
						parent->second->stableJointId);
					const auto childJoint = skeletonJointIndices.find(
						child->second->stableJointId);
					if(parentJoint != skeletonJointIndices.end()
						&& childJoint != skeletonJointIndices.end()
						&& !IsAncestor(parentJoint->second, childJoint->second, skeleton)) {
						builder.Add(RagdollProfileDiagnosticCode::ConstraintSkeletonOrder,
							AnimationStatus::Rejected,
							"The constraint parent joint is not an ancestor of the child joint.",
							"Orient the constraint graph in skeleton parent-to-child order.",
							child->first, constraint.constraintId,
							child->second->stableJointId);
					}
					if(!constraint.disableCollisionBetweenBodies
						&& parent->second->collisionLayer < 32U
						&& child->second->collisionLayer < 32U) {
						const bool parentSeesChild = (parent->second->collisionMask
							& (1U << child->second->collisionLayer)) != 0U;
						const bool childSeesParent = (child->second->collisionMask
							& (1U << parent->second->collisionLayer)) != 0U;
						if(!parentSeesChild || !childSeesParent) {
							builder.Add(RagdollProfileDiagnosticCode::ConstrainedPairCannotCollide,
								AnimationStatus::Rejected,
								"A colliding constraint pair is excluded by its body masks.",
								"Enable both layer directions or disable collision on the constraint.",
								child->first, constraint.constraintId);
						}
					}
				}
				if(!IsValidFrame(constraint.parentFrame)
					|| !IsValidFrame(constraint.childFrame)) {
					builder.Add(RagdollProfileDiagnosticCode::InvalidConstraintFrame,
						AnimationStatus::Rejected,
						"A constraint local frame has non-finite, non-normalized, or parallel axes.",
						"Author normalized orthogonal local axes and orientations.",
						0U, constraint.constraintId);
				}
				for(std::size_t axis = 0U; axis < constraint.limits.size(); ++axis) {
					const RagdollAxisLimit& limit = constraint.limits[axis];
					const RagdollAxisMotor& motor = constraint.motors[axis];
					if(!IsValidLimit(limit)) {
						builder.Add(RagdollProfileDiagnosticCode::InvalidAxisLimit,
							AnimationStatus::Rejected,
							"A per-axis constraint limit is invalid.",
							"Use finite ordered limits and non-negative spring settings.",
							0U, constraint.constraintId);
					}
					if(!IsValidMotor(motor)) {
						builder.Add(RagdollProfileDiagnosticCode::InvalidMotor,
							AnimationStatus::Rejected,
							"A per-axis motor contains invalid target or drive values.",
							"Use finite targets and non-negative stiffness, damping, and force.",
							0U, constraint.constraintId);
					} else if(motor.mode != RagdollMotorMode::Disabled
						&& motor.maximumForce <= 0.0F) {
						builder.Add(RagdollProfileDiagnosticCode::MotorMaximumForceMissing,
							AnimationStatus::Rejected,
							"An enabled motor has no positive maximum force.",
							"Set a bounded positive maximum force for the axis.",
							0U, constraint.constraintId);
					}
					if(motor.mode == RagdollMotorMode::Position
						&& limit.mode == RagdollLimitMode::Limited
						&& (motor.targetPosition < limit.minimum
							|| motor.targetPosition > limit.maximum)) {
						builder.Add(RagdollProfileDiagnosticCode::InvalidMotor,
							AnimationStatus::Rejected,
							"A position motor target lies outside its enabled axis limit.",
							"Move the target inside the authored limit range.",
							0U, constraint.constraintId);
					}
				}
			}
			for(auto& [bodyId, childIds] : children) {
				static_cast<void>(bodyId);
				std::sort(childIds.begin(), childIds.end());
			}
			for(const auto& [bodyId, inbound] : inboundCounts) {
				if(inbound == 0U) {
					report.rootBodyIds.push_back(bodyId);
				}
			}

			std::set<std::uint64_t> graphVisited;
			std::set<std::uint64_t> graphActive;
			std::uint64_t cycleBodyId = 0U;
			for(const auto& [bodyId, body] : bodiesById) {
				static_cast<void>(body);
				if(!graphVisited.contains(bodyId)
					&& !VisitConstraintGraph(bodyId, children, graphVisited,
						graphActive, cycleBodyId)) {
					builder.Add(RagdollProfileDiagnosticCode::ConstraintCycle,
						AnimationStatus::Rejected,
						"The constraint graph contains a cycle.",
						"Remove one cyclic parent-child link.", cycleBodyId);
					break;
				}
			}
			if(!asset.allowDisconnectedBodyIslands && !bodiesById.empty()) {
				std::set<std::uint64_t> connected;
				std::set<std::uint64_t> active;
				std::uint64_t ignoredCycle = 0U;
				if(bodiesById.contains(asset.rootBodyId)) {
					VisitConstraintGraph(asset.rootBodyId, children, connected,
						active, ignoredCycle);
				}
				if(report.rootBodyIds.size() != 1U
					|| connected.size() != bodiesById.size()
					|| (!report.rootBodyIds.empty()
						&& report.rootBodyIds.front() != asset.rootBodyId)) {
					builder.Add(RagdollProfileDiagnosticCode::DisconnectedConstraintGraph,
						AnimationStatus::Rejected,
						"The body graph is not one tree rooted at the declared root body.",
						"Connect every body once beneath the declared root, or explicitly allow islands.",
						asset.rootBodyId);
				}
			}

			std::map<std::uint32_t, RagdollCollisionLayerRule> layerRules;
			std::uint32_t enabledLayerMask = 0U;
			for(const RagdollCollisionLayerRule& layer : options.collisionLayers) {
				if(layer.layer >= 32U || !layerRules.emplace(layer.layer, layer).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateCollisionLayer,
						AnimationStatus::InvalidArgument,
						"Collision-layer validation contains a duplicate or out-of-range layer.",
						"Provide one definition for each layer index from zero through thirty-one.");
					continue;
				}
				if(layer.enabled) {
					enabledLayerMask |= 1U << layer.layer;
				}
			}
			for(const RagdollBodyRecord& body : asset.bodies) {
				if(body.collisionLayer >= 32U) {
					builder.Add(RagdollProfileDiagnosticCode::CollisionLayerMissing,
						AnimationStatus::Rejected,
						"A body collision layer is outside the supported zero-through-thirty-one range.",
						"Select a valid project collision layer.", body.bodyId);
					continue;
				}
				const auto layer = layerRules.find(body.collisionLayer);
				if(layer == layerRules.end()) {
					if(options.requireCollisionLayerDefinitions) {
						builder.Add(RagdollProfileDiagnosticCode::CollisionLayerMissing,
							AnimationStatus::Rejected,
							"A body collision layer has no project-layer definition.",
							"Add the layer definition or select an existing enabled layer.",
							body.bodyId);
					}
					continue;
				}
				if(!layer->second.enabled) {
					builder.Add(RagdollProfileDiagnosticCode::CollisionLayerDisabled,
						AnimationStatus::Rejected,
						"A body uses a disabled collision layer.",
						"Enable the layer or choose another layer.", body.bodyId);
				}
				if((body.collisionMask & ~layer->second.allowedMask) != 0U
					|| (body.collisionMask & ~enabledLayerMask) != 0U
					|| (body.collisionMask & ~asset.allowedCollisionLayerMask) != 0U) {
					builder.Add(RagdollProfileDiagnosticCode::CollisionMaskOutsideLayerPolicy,
						AnimationStatus::Rejected,
						"A body collision mask includes a layer forbidden by project or profile policy.",
						"Restrict the mask to enabled layers allowed by both policies.", body.bodyId);
				}
			}
			if(asset.requireSymmetricCollisionFilters) {
				for(std::size_t leftIndex = 0U; leftIndex < asset.bodies.size(); ++leftIndex) {
					const RagdollBodyRecord& left = asset.bodies[leftIndex];
					for(std::size_t rightIndex = leftIndex + 1U;
						rightIndex < asset.bodies.size(); ++rightIndex) {
						const RagdollBodyRecord& right = asset.bodies[rightIndex];
						if(left.collisionLayer >= 32U || right.collisionLayer >= 32U) {
							continue;
						}
						const bool leftSeesRight =
							(left.collisionMask & (1U << right.collisionLayer)) != 0U;
						const bool rightSeesLeft =
							(right.collisionMask & (1U << left.collisionLayer)) != 0U;
						if(leftSeesRight != rightSeesLeft) {
							builder.Add(RagdollProfileDiagnosticCode::AsymmetricCollisionFilter,
								AnimationStatus::Rejected,
								"Two bodies disagree about whether their layers may collide.",
								"Make both collision-mask directions agree.",
								std::min(left.bodyId, right.bodyId));
						}
					}
				}
			}

			if(std::isfinite(report.totalMassKilograms)
				&& (report.totalMassKilograms < options.minimumTotalMassKilograms
					|| report.totalMassKilograms > options.maximumTotalMassKilograms)) {
				builder.Add(RagdollProfileDiagnosticCode::TotalMassOutsideExpectedRange,
					AnimationStatus::Rejected,
					"The total ragdoll mass is outside the accepted validation range.",
					"Adjust body masses or the project validation range.");
			}
			if(asset.expectedTotalMassKilograms > 0.0F
				&& std::isfinite(report.totalMassKilograms)) {
				const float tolerance = asset.expectedTotalMassKilograms
					* asset.expectedMassToleranceFraction;
				if(std::abs(report.totalMassKilograms - asset.expectedTotalMassKilograms)
					> tolerance) {
					builder.Add(RagdollProfileDiagnosticCode::TotalMassOutsideExpectedRange,
						AnimationStatus::Rejected,
						"The total body mass differs from the authored target beyond tolerance.",
						"Redistribute mass while retaining the authored total.");
				}
			}
			if(asset.bodies.size() > 1U && report.totalMassKilograms > 0.0F) {
				for(const RagdollBodyRecord& body : asset.bodies) {
					const float fraction = body.massKilograms / report.totalMassKilograms;
					if(std::isfinite(fraction)
						&& (fraction < asset.minimumBodyMassFraction
							|| fraction > asset.maximumBodyMassFraction)) {
						builder.Add(RagdollProfileDiagnosticCode::BodyMassDistribution,
							AnimationStatus::Rejected,
							"A body mass fraction falls outside the authored distribution range.",
							"Redistribute mass so no body is implausibly light or dominant.",
							body.bodyId);
					}
				}
			}

			std::set<std::uint64_t> blendJoints;
			for(const RagdollBlendRecord& blend : asset.blends) {
				if(!blendJoints.insert(blend.stableJointId).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateBlendJoint,
						AnimationStatus::Rejected,
						"More than one blend record targets the same stable joint.",
						"Keep one blend policy per joint.", 0U, 0U, blend.stableJointId);
				}
				if(!skeletonJointIndices.contains(blend.stableJointId)) {
					builder.Add(RagdollProfileDiagnosticCode::UnknownBlendJoint,
						AnimationStatus::IncompatibleAsset,
						"A blend record targets a joint absent from the skeleton.",
						"Select a stable joint ID from the current skeleton.",
						0U, 0U, blend.stableJointId);
				}
			}

			std::set<std::uint64_t> recoveryIds;
			for(const RagdollRecoveryRecord& recovery : asset.recovery) {
				if(!recoveryIds.insert(recovery.candidateId).second) {
					builder.Add(RagdollProfileDiagnosticCode::DuplicateRecoveryCandidate,
						AnimationStatus::Rejected,
						"Two recovery records use the same stable candidate ID.",
						"Assign a unique recovery candidate ID.",
						0U, 0U, 0U, recovery.candidateId);
				}
				if(recovery.clipSkeletonFingerprint != skeleton.compatibilityFingerprint) {
					builder.Add(RagdollProfileDiagnosticCode::RecoveryClipIncompatible,
						AnimationStatus::IncompatibleAsset,
						"A recovery clip was authored for another skeleton fingerprint.",
						"Choose or retarget a recovery clip for this skeleton.",
						0U, 0U, 0U, recovery.candidateId);
				}
				if(recovery.requiredReferencePoseHash != skeleton.referencePoseHash) {
					builder.Add(RagdollProfileDiagnosticCode::RecoveryReferencePoseMismatch,
						AnimationStatus::IncompatibleAsset,
						"A recovery candidate names a different reference pose.",
						"Reauthor its root/contact alignment from the current reference pose.",
						0U, 0U, 0U, recovery.candidateId);
				}
				if(!skeletonJointIndices.contains(recovery.rootJointStableId)) {
					builder.Add(RagdollProfileDiagnosticCode::RecoveryRootJointMissing,
						AnimationStatus::IncompatibleAsset,
						"A recovery root joint is absent from the skeleton.",
						"Select a valid stable root joint.",
						0U, 0U, recovery.rootJointStableId, recovery.candidateId);
				}
				std::set<std::uint64_t> contacts;
				for(const std::uint64_t jointId : recovery.contactJointStableIds) {
					if(!contacts.insert(jointId).second
						|| !skeletonJointIndices.contains(jointId)) {
						builder.Add(RagdollProfileDiagnosticCode::RecoveryContactJointMissing,
							AnimationStatus::IncompatibleAsset,
							"A recovery contact joint is duplicated or absent from the skeleton.",
							"Use unique stable contact joints from the current skeleton.",
							0U, 0U, jointId, recovery.candidateId);
					}
				}
				if(!IsValidRecoveryMetadata(recovery)) {
					builder.Add(RagdollProfileDiagnosticCode::InvalidRecoveryMetadata,
						AnimationStatus::Rejected,
						"A recovery candidate has invalid posture, alignment, speed, blend, or clearance metadata.",
						"Author finite bounded recovery metadata and a normalized alignment orientation.",
						0U, 0U, 0U, recovery.candidateId);
				}
			}

			builder.Finish();
			return report;
		} catch(const std::bad_alloc&) {
			RagdollProfileValidationReport failure;
			failure.status = AnimationStatus::InternalError;
			return failure;
		} catch(...) {
			RagdollProfileValidationReport failure;
			failure.status = AnimationStatus::InternalError;
			return failure;
		}
	}

	RagdollProfileValidationReport ValidateRagdollProfile(
		const RagdollProfileAsset& asset,
		AnimationCompatibilityFingerprint skeletonFingerprint) noexcept {
		try {
			RagdollProfileValidationReport report;
			ReportBuilder builder(report);
			const AnimationStatus structure = ValidateRagdollProfileAssetStructure(asset);
			if(structure != AnimationStatus::Ok) {
				builder.Add(RagdollProfileDiagnosticCode::InvalidStructure,
					structure,
					"The ragdoll profile contains an invalid or out-of-bounds record.",
					"Correct the profile before publication.");
			}
			if(asset.skeletonFingerprint != skeletonFingerprint) {
				builder.Add(RagdollProfileDiagnosticCode::SkeletonFingerprintMismatch,
					AnimationStatus::IncompatibleAsset,
					"The profile was authored for another skeleton fingerprint.",
					"Validate against the complete current SkeletonAsset.");
			}
			for(const RagdollBodyRecord& body : asset.bodies) {
				if(std::isfinite(body.massKilograms) && body.massKilograms > 0.0F) {
					report.totalMassKilograms += body.massKilograms;
				}
			}
			builder.Finish();
			return report;
		} catch(...) {
			RagdollProfileValidationReport failure;
			failure.status = AnimationStatus::InternalError;
			return failure;
		}
	}
}
