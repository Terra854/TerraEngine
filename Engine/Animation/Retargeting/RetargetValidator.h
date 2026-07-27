// RetargetValidator.h - Quantitative, deterministic retarget publication checks.

#pragma once

#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Animation {
	enum class RetargetDiagnosticSeverity : std::uint8_t {
		Information = 0U,
		Warning = 1U,
		Error = 2U
	};

	// Codes are stable diagnostic categories. The validator sorts by code and
	// stable element identity so worker completion order never changes output.
	enum class RetargetDiagnosticCode : std::uint16_t {
		MissingMapping = 0U,
		AmbiguousMapping = 1U,
		CyclicMapping = 2U,
		DuplicateMapping = 3U,
		IncompatibleSkeleton = 4U,
		UnsupportedMapping = 5U,
		InvalidHierarchy = 6U,
		InvalidBasis = 7U,
		InvalidScale = 8U,
		InvalidContact = 9U,
		InvalidTwist = 10U,
		MissingFallback = 11U,
		ReducedFidelity = 12U,
		HardLimitExceeded = 13U,
		NonFiniteValue = 14U
	};

	struct RetargetValidationDiagnostic {
		RetargetDiagnosticCode code = RetargetDiagnosticCode::MissingMapping;
		RetargetDiagnosticSeverity severity = RetargetDiagnosticSeverity::Error;
		std::uint64_t elementId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		float measuredValue = 0.0F;
		float acceptedValue = 0.0F;
		std::string sourceRole;
		std::string targetRole;
		std::string message;

		auto operator<=>(const RetargetValidationDiagnostic&) const = default;
	};

	struct RetargetValidationMetrics {
		std::uint32_t requiredMappingCount = 0U;
		std::uint32_t resolvedRequiredMappingCount = 0U;
		std::uint32_t optionalMappingCount = 0U;
		std::uint32_t resolvedOptionalMappingCount = 0U;
		std::uint32_t semanticMappingCount = 0U;
		std::uint32_t explicitMappingCount = 0U;
		std::uint32_t chainCount = 0U;
		std::uint32_t contactCount = 0U;
		std::uint32_t twistJointCount = 0U;
		std::uint32_t fallbackCount = 0U;
		float requiredCoverage = 0.0F;
		float optionalCoverage = 0.0F;
		float sourceToTargetHeightRatio = 1.0F;
		float maximumContactToleranceMeters = 0.0F;
		float qualityScore = 0.0F;

		auto operator<=>(const RetargetValidationMetrics&) const = default;
	};

	struct RetargetValidationResult {
		AnimationStatus status = AnimationStatus::NotReady;
		bool publishable = false;
		RetargetValidationMetrics metrics{};
		std::vector<RetargetValidationDiagnostic> diagnostics;

		auto operator<=>(const RetargetValidationResult&) const = default;
	};

	// The compiled mapping stores indices resolved against exactly the skeleton
	// fingerprints named by the profile. Runtime workers need no name lookup or
	// access to mutable editor rig data.
	struct RetargetCompiledJointMapping {
		std::uint64_t mappingId = 0U;
		std::uint64_t parentMappingId = 0U;
		std::uint64_t fallbackRuleId = 0U;
		std::uint32_t sourceJointIndex = 0U;
		std::uint32_t targetJointIndex = 0U;
		AnimationQuaternion sourceBasis{};
		AnimationQuaternion targetBasis{};
		RetargetMappingKind kind = RetargetMappingKind::Explicit;
		bool required = true;

		auto operator<=>(const RetargetCompiledJointMapping&) const = default;
	};

	struct RetargetCompiledChain {
		std::uint64_t chainId = 0U;
		std::vector<std::uint32_t> sourceJointIndices;
		std::vector<std::uint32_t> targetJointIndices;
		RetargetTranslationScaleMode translationScaleMode =
			RetargetTranslationScaleMode::PerChainLength;
		float authoredTranslationScale = 1.0F;
		bool required = true;

		auto operator<=>(const RetargetCompiledChain&) const = default;
	};

	struct RetargetCompiledProfile {
		RetargetProfileAsset profile{};
		RetargetValidationMetrics metrics{};
		std::vector<RetargetCompiledJointMapping> mappings;
		std::vector<RetargetCompiledChain> chains;
		std::uint64_t stableHash = 0U;

		auto operator<=>(const RetargetCompiledProfile&) const = default;
	};

	// Both functions stage complete results and replace their outputs only after
	// all fallible allocations and compatibility checks succeed.
	TERRAENGINE_API AnimationStatus ValidateRetargetProfile(
		const RetargetProfileAsset& profile,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		RetargetValidationResult& output) noexcept;
	TERRAENGINE_API AnimationStatus CompileRetargetProfile(
		const RetargetProfileAsset& profile,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		RetargetValidationResult& validationOutput,
		RetargetCompiledProfile& compiledOutput) noexcept;
}
