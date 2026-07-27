// RagdollProfileValidator.h - Deterministic skeleton and physics-policy checks.

#pragma once

#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace TerraEngine::Animation {
	// Diagnostic codes are persisted by editor/automation evidence. Append new
	// values without reordering so two runs can compare reports deterministically.
	enum class RagdollProfileDiagnosticCode : std::uint16_t {
		InvalidStructure = 0U,
		SkeletonAssetMismatch = 1U,
		SkeletonFingerprintMismatch = 2U,
		ReferencePoseMismatch = 3U,
		DuplicateBodyId = 4U,
		DuplicateBodyJoint = 5U,
		UnknownBodyJoint = 6U,
		MissingRequiredBody = 7U,
		RootBodyMissing = 8U,
		DuplicateColliderId = 9U,
		InvalidCollider = 10U,
		DuplicateConstraintId = 11U,
		DuplicateConstraintPair = 12U,
		UnknownConstraintBody = 13U,
		MultipleParentConstraints = 14U,
		ConstraintCycle = 15U,
		DisconnectedConstraintGraph = 16U,
		ConstraintSkeletonOrder = 17U,
		InvalidConstraintFrame = 18U,
		InvalidAxisLimit = 19U,
		InvalidMotor = 20U,
		MotorMaximumForceMissing = 21U,
		CollisionLayerMissing = 22U,
		CollisionLayerDisabled = 23U,
		CollisionMaskOutsideLayerPolicy = 24U,
		AsymmetricCollisionFilter = 25U,
		ConstrainedPairCannotCollide = 26U,
		InvalidBodyMass = 27U,
		TotalMassOutsideExpectedRange = 28U,
		BodyMassDistribution = 29U,
		DuplicateBlendJoint = 30U,
		UnknownBlendJoint = 31U,
		DuplicateRecoveryCandidate = 32U,
		RecoveryClipIncompatible = 33U,
		RecoveryReferencePoseMismatch = 34U,
		RecoveryRootJointMissing = 35U,
		RecoveryContactJointMissing = 36U,
		InvalidRecoveryMetadata = 37U,
		DuplicateCollisionLayer = 38U
	};

	enum class RagdollProfileDiagnosticSeverity : std::uint8_t {
		Warning = 0U,
		Error = 1U
	};

	struct RagdollCollisionLayerRule {
		std::uint32_t layer = 0U;
		std::uint32_t allowedMask = 0xffffffffU;
		bool enabled = true;

		auto operator<=>(const RagdollCollisionLayerRule&) const = default;
	};

	struct RagdollProfileValidationOptions {
		std::span<const RagdollCollisionLayerRule> collisionLayers{};
		float minimumTotalMassKilograms = 0.1F;
		float maximumTotalMassKilograms = 1000.0F;
		bool requireCollisionLayerDefinitions = true;
	};

	struct RagdollProfileDiagnostic {
		RagdollProfileDiagnosticCode code =
			RagdollProfileDiagnosticCode::InvalidStructure;
		AnimationStatus status = AnimationStatus::Rejected;
		RagdollProfileDiagnosticSeverity severity =
			RagdollProfileDiagnosticSeverity::Error;
		std::uint64_t bodyId = 0U;
		std::uint64_t constraintId = 0U;
		std::uint64_t stableJointId = 0U;
		std::uint64_t recoveryCandidateId = 0U;
		std::string_view message;
		std::string_view correctiveAction;

		auto operator<=>(const RagdollProfileDiagnostic&) const = default;
	};

	struct RagdollProfileValidationReport {
		AnimationStatus status = AnimationStatus::Ok;
		float totalMassKilograms = 0.0F;
		std::vector<std::uint64_t> rootBodyIds;
		std::vector<std::uint64_t> missingRequiredJointIds;
		std::vector<RagdollProfileDiagnostic> diagnostics;

		bool IsValid() const noexcept {
			return status == AnimationStatus::Ok;
		}
	};

	// Validation is a pure worker-safe operation. The returned report owns all
	// variable storage, and no live skeleton/profile state is changed on failure.
	TERRAENGINE_API RagdollProfileValidationReport ValidateRagdollProfile(
		const RagdollProfileAsset& asset,
		const SkeletonAsset& skeleton,
		RagdollProfileValidationOptions options = {}) noexcept;

	// This compatibility overload retains the early fingerprint-only call site.
	// New publication paths must use the SkeletonAsset overload above so mapping,
	// topology, coverage, and recovery joints are actually checked.
	TERRAENGINE_API RagdollProfileValidationReport ValidateRagdollProfile(
		const RagdollProfileAsset& asset,
		AnimationCompatibilityFingerprint skeletonFingerprint) noexcept;
}
