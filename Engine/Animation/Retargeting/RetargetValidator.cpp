// RetargetValidator.cpp - Fail-closed retarget mapping validation and compile.

#include "Animation/Retargeting/RetargetValidator.h"

#include "Animation/Procedural/ProceduralTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <tuple>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		struct MappingWork {
			std::uint64_t mappingId = 0U;
			std::uint64_t parentMappingId = 0U;
			std::uint64_t basisId = 0U;
			std::uint64_t fallbackRuleId = 0U;
			std::uint64_t sourceJointId = 0U;
			std::uint64_t targetJointId = 0U;
			std::uint32_t sourceJointIndex = 0U;
			std::uint32_t targetJointIndex = 0U;
			std::string sourceRole;
			std::string targetRole;
			RetargetMappingKind kind = RetargetMappingKind::Explicit;
			bool required = true;
			bool sourceResolved = false;
			bool targetResolved = false;
		};

		void AddDiagnostic(
			RetargetValidationResult& result,
			RetargetDiagnosticCode code,
			RetargetDiagnosticSeverity severity,
			std::uint64_t elementId,
			std::uint64_t sourceJointId,
			std::uint64_t targetJointId,
			std::string sourceRole,
			std::string targetRole,
			std::string message,
			float measuredValue = 0.0F,
			float acceptedValue = 0.0F) {
			result.diagnostics.push_back({code, severity, elementId, sourceJointId,
				targetJointId, measuredValue, acceptedValue, std::move(sourceRole),
				std::move(targetRole), std::move(message)});
		}

		bool DiagnosticLess(
			const RetargetValidationDiagnostic& left,
			const RetargetValidationDiagnostic& right) noexcept {
			return std::tie(left.code, left.severity, left.elementId, left.sourceJointId,
				left.targetJointId, left.sourceRole, left.targetRole, left.message)
				< std::tie(right.code, right.severity, right.elementId,
					right.sourceJointId, right.targetJointId, right.sourceRole,
					right.targetRole, right.message);
		}

		std::optional<std::uint32_t> FindJointIndex(
			const SkeletonAsset& skeleton,
			std::uint64_t stableJointId) noexcept {
			if(stableJointId == 0U) {
				return std::nullopt;
			}
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				if(skeleton.joints[index].stableId == stableJointId) {
					return static_cast<std::uint32_t>(index);
				}
			}
			return std::nullopt;
		}

		std::vector<std::uint32_t> FindSemanticMatches(
			const SkeletonAsset& skeleton,
			const std::string& semantic) {
			std::vector<std::uint32_t> matches;
			if(semantic.empty()) {
				return matches;
			}
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				if(skeleton.joints[index].semanticRole == semantic) {
					matches.push_back(static_cast<std::uint32_t>(index));
				}
			}
			return matches;
		}

		bool IsDescendant(
			const SkeletonAsset& skeleton,
			std::uint32_t descendant,
			std::uint32_t ancestor) noexcept {
			if(descendant >= skeleton.joints.size() || ancestor >= skeleton.joints.size()) {
				return false;
			}
			std::uint32_t current = descendant;
			for(std::size_t depth = 0U; depth <= skeleton.joints.size(); ++depth) {
				if(current == ancestor) {
					return true;
				}
				const std::int32_t parent = skeleton.joints[current].parentIndex;
				if(parent < 0 || static_cast<std::size_t>(parent) >= skeleton.joints.size()) {
					return false;
				}
				current = static_cast<std::uint32_t>(parent);
			}
			return false;
		}

		bool SkeletonTopologyIsUsable(
			const SkeletonAsset& skeleton,
			RetargetValidationResult& result,
			bool source) {
			if(skeleton.joints.empty()
				|| skeleton.joints.size() > kRetargetProfileMaximumMappings) {
				AddDiagnostic(result, RetargetDiagnosticCode::HardLimitExceeded,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U,
					source ? "source skeleton" : "target skeleton", {},
					"Skeleton joint count is empty or exceeds the retarget version-1 bound.",
					static_cast<float>(skeleton.joints.size()),
					static_cast<float>(kRetargetProfileMaximumMappings));
				return false;
			}
			std::vector<std::uint64_t> ids;
			ids.reserve(skeleton.joints.size());
			std::size_t rootCount = 0U;
			bool usable = true;
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				const SkeletonJoint& joint = skeleton.joints[index];
				ids.push_back(joint.stableId);
				if(joint.parentIndex < 0) {
					++rootCount;
				} else if(static_cast<std::size_t>(joint.parentIndex) >= index) {
					usable = false;
					AddDiagnostic(result, RetargetDiagnosticCode::InvalidHierarchy,
						RetargetDiagnosticSeverity::Error, joint.stableId,
						source ? joint.stableId : 0U,
						source ? 0U : joint.stableId,
						joint.semanticRole, joint.semanticRole,
						"Skeleton joints must be parent-before-child and acyclic.");
				}
				if(joint.stableId == 0U || !IsFinite(joint.localBindTransform)
					|| !IsNormalized(joint.localBindTransform.rotation, 0.01F)) {
					usable = false;
					AddDiagnostic(result, RetargetDiagnosticCode::NonFiniteValue,
						RetargetDiagnosticSeverity::Error, joint.stableId,
						source ? joint.stableId : 0U,
						source ? 0U : joint.stableId,
						joint.semanticRole, joint.semanticRole,
						"Skeleton joint identity and bind transform must be finite and normalized.");
				}
			}
			std::sort(ids.begin(), ids.end());
			if(std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
				usable = false;
				AddDiagnostic(result, RetargetDiagnosticCode::AmbiguousMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U,
					source ? "source stable joint IDs" : "target stable joint IDs", {},
					"Skeleton contains duplicate stable joint IDs, so mappings are ambiguous.");
			}
			if(rootCount != 1U) {
				usable = false;
				AddDiagnostic(result, RetargetDiagnosticCode::InvalidHierarchy,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U,
					source ? "source root" : "target root", {},
					"Retargeting requires exactly one logical skeleton root.",
					static_cast<float>(rootCount), 1.0F);
			}
			return usable;
		}

		void ResolveSemanticSide(
			const SkeletonAsset& skeleton,
			const std::string& semantic,
			std::uint64_t authoredJointId,
			bool source,
			MappingWork& mapping,
			RetargetValidationResult& result) {
			const std::vector<std::uint32_t> matches = FindSemanticMatches(skeleton, semantic);
			if(matches.empty()) {
				AddDiagnostic(result, RetargetDiagnosticCode::MissingMapping,
					mapping.required ? RetargetDiagnosticSeverity::Error
						: RetargetDiagnosticSeverity::Warning,
					mapping.mappingId, source ? authoredJointId : 0U,
					source ? 0U : authoredJointId,
					source ? semantic : std::string{},
					source ? std::string{} : semantic,
					source
						? "Source semantic does not resolve to a joint."
						: "Target semantic does not resolve to a joint.");
				return;
			}
			if(matches.size() != 1U) {
				AddDiagnostic(result, RetargetDiagnosticCode::AmbiguousMapping,
					RetargetDiagnosticSeverity::Error, mapping.mappingId,
					source ? authoredJointId : 0U, source ? 0U : authoredJointId,
					source ? semantic : std::string{},
					source ? std::string{} : semantic,
					"Semantic resolves to more than one skeleton joint.",
					static_cast<float>(matches.size()), 1.0F);
				return;
			}
			const std::uint32_t index = matches.front();
			if(authoredJointId != 0U && skeleton.joints[index].stableId != authoredJointId) {
				AddDiagnostic(result, RetargetDiagnosticCode::IncompatibleSkeleton,
					RetargetDiagnosticSeverity::Error, mapping.mappingId,
					source ? authoredJointId : 0U, source ? 0U : authoredJointId,
					source ? semantic : std::string{},
					source ? std::string{} : semantic,
					"Authored semantic and stable joint ID resolve to different joints.");
				return;
			}
			if(source) {
				mapping.sourceJointIndex = index;
				mapping.sourceJointId = skeleton.joints[index].stableId;
				mapping.sourceResolved = true;
			} else {
				mapping.targetJointIndex = index;
				mapping.targetJointId = skeleton.joints[index].stableId;
				mapping.targetResolved = true;
			}
		}

		void ResolveExplicitSide(
			const SkeletonAsset& skeleton,
			std::uint64_t jointId,
			bool source,
			MappingWork& mapping,
			RetargetValidationResult& result) {
			const std::optional<std::uint32_t> index = FindJointIndex(skeleton, jointId);
			if(!index.has_value()) {
				AddDiagnostic(result, RetargetDiagnosticCode::MissingMapping,
					mapping.required ? RetargetDiagnosticSeverity::Error
						: RetargetDiagnosticSeverity::Warning,
					mapping.mappingId, source ? jointId : 0U, source ? 0U : jointId,
					{}, {}, source
						? "Explicit source joint ID is missing from the source skeleton."
						: "Explicit target joint ID is missing from the target skeleton.");
				return;
			}
			if(source) {
				mapping.sourceJointIndex = *index;
				mapping.sourceResolved = true;
			} else {
				mapping.targetJointIndex = *index;
				mapping.targetResolved = true;
			}
		}

		const MappingWork* FindMapping(
			const std::vector<MappingWork>& mappings,
			std::uint64_t mappingId) noexcept {
			const auto iterator = std::find_if(mappings.begin(), mappings.end(),
				[mappingId](const MappingWork& mapping) {
					return mapping.mappingId == mappingId;
				});
			return iterator != mappings.end() ? &*iterator : nullptr;
		}

		const RetargetOrientationBasis* FindBasis(
			const RetargetProfileAsset& profile,
			std::uint64_t basisId) noexcept {
			const auto iterator = std::find_if(profile.orientationBases.begin(),
				profile.orientationBases.end(), [basisId](const RetargetOrientationBasis& basis) {
					return basis.basisId == basisId;
				});
			return iterator != profile.orientationBases.end() ? &*iterator : nullptr;
		}

		const RetargetFallbackRule* FindFallback(
			const RetargetProfileAsset& profile,
			std::uint64_t fallbackRuleId) noexcept {
			const auto iterator = std::find_if(profile.fallbackRules.begin(),
				profile.fallbackRules.end(), [fallbackRuleId](const RetargetFallbackRule& fallback) {
					return fallback.fallbackRuleId == fallbackRuleId;
				});
			return iterator != profile.fallbackRules.end() ? &*iterator : nullptr;
		}

		float SkeletonHeight(const SkeletonAsset& skeleton) {
			// Allocation failure propagates to the public staging boundary instead of
			// silently publishing metrics computed from an invented fallback height.
			std::vector<AnimationTransform> model(skeleton.joints.size());
			float minimumY = std::numeric_limits<float>::max();
			float maximumY = std::numeric_limits<float>::lowest();
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				const SkeletonJoint& joint = skeleton.joints[index];
				if(joint.parentIndex < 0) {
					model[index] = joint.localBindTransform;
				} else {
					const std::size_t parentIndex =
						static_cast<std::size_t>(joint.parentIndex);
					// Topology diagnostics do not stop the remaining deterministic
					// report. Keep metric collection safe for malformed parent indices.
					if(parentIndex >= index || parentIndex >= model.size()) {
						return 0.0F;
					}
					model[index] = ProceduralMath::Combine(model[parentIndex],
						joint.localBindTransform);
				}
				if(!IsFinite(model[index])) {
					return 0.0F;
				}
				minimumY = std::min(minimumY, model[index].translation.y);
				maximumY = std::max(maximumY, model[index].translation.y);
			}
			const float height = maximumY - minimumY;
			return std::isfinite(height) && height > 1.0e-6F ? height : 0.0F;
		}

		std::uint64_t HashCompiledBytes(std::span<const std::uint8_t> bytes) noexcept {
			std::uint64_t hash = 1469598103934665603ULL;
			for(const std::uint8_t byte : bytes) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		AnimationStatus FinalStatus(const RetargetValidationResult& result) noexcept {
			bool hasError = false;
			bool incompatible = false;
			bool unsupported = false;
			bool hardLimit = false;
			for(const RetargetValidationDiagnostic& diagnostic : result.diagnostics) {
				hasError = hasError || diagnostic.severity == RetargetDiagnosticSeverity::Error;
				incompatible = incompatible
					|| diagnostic.code == RetargetDiagnosticCode::IncompatibleSkeleton;
				unsupported = unsupported
					|| diagnostic.code == RetargetDiagnosticCode::UnsupportedMapping;
				hardLimit = hardLimit
					|| diagnostic.code == RetargetDiagnosticCode::HardLimitExceeded;
			}
			if(!hasError) {
				return AnimationStatus::Ok;
			}
			if(hardLimit) {
				return AnimationStatus::HardLimitExceeded;
			}
			if(unsupported) {
				return AnimationStatus::Unsupported;
			}
			if(incompatible) {
				return AnimationStatus::IncompatibleAsset;
			}
			return AnimationStatus::Rejected;
		}

		AnimationStatus RunValidation(
			const RetargetProfileAsset& profile,
			const SkeletonAsset& sourceSkeleton,
			const SkeletonAsset& targetSkeleton,
			RetargetValidationResult& result,
			RetargetCompiledProfile* compiled) {
			const AnimationStatus structural = ValidateRetargetProfileAssetStructure(profile);
			if(structural == AnimationStatus::HardLimitExceeded) {
				AddDiagnostic(result, RetargetDiagnosticCode::HardLimitExceeded,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Retarget profile exceeds a version-1 bounded count or payload limit.");
			} else if(structural == AnimationStatus::Unsupported
				|| structural == AnimationStatus::UnsupportedMajorVersion) {
				AddDiagnostic(result, RetargetDiagnosticCode::UnsupportedMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Retarget profile contains an unsupported format or policy value.");
			} else if(structural != AnimationStatus::Ok) {
				AddDiagnostic(result, RetargetDiagnosticCode::NonFiniteValue,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Retarget profile contains a missing identity, non-finite value, or invalid tolerance.");
			}
			if(structural == AnimationStatus::HardLimitExceeded
				|| structural == AnimationStatus::Unsupported
				|| structural == AnimationStatus::UnsupportedMajorVersion) {
				std::sort(result.diagnostics.begin(), result.diagnostics.end(), DiagnosticLess);
				result.status = FinalStatus(result);
				result.publishable = false;
				return result.status;
			}

			if(profile.sourceSkeletonAssetId != sourceSkeleton.assetId
				|| profile.sourceSkeletonFingerprint != sourceSkeleton.compatibilityFingerprint
				|| profile.sourceReferencePoseHash != sourceSkeleton.referencePoseHash) {
				AddDiagnostic(result, RetargetDiagnosticCode::IncompatibleSkeleton,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U,
					"source skeleton", {},
					"Source skeleton identity, compatibility fingerprint, or reference-pose fingerprint does not match the profile.");
			}
			if(profile.targetSkeletonAssetId != targetSkeleton.assetId
				|| profile.targetSkeletonFingerprint != targetSkeleton.compatibilityFingerprint
				|| profile.targetReferencePoseHash != targetSkeleton.referencePoseHash) {
				AddDiagnostic(result, RetargetDiagnosticCode::IncompatibleSkeleton,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U,
					{}, "target skeleton",
					"Target skeleton identity, compatibility fingerprint, or reference-pose fingerprint does not match the profile.");
			}

			static_cast<void>(SkeletonTopologyIsUsable(sourceSkeleton, result, true));
			static_cast<void>(SkeletonTopologyIsUsable(targetSkeleton, result, false));

			std::vector<MappingWork> mappings;
			mappings.reserve(profile.semanticMappings.size() + profile.explicitMappings.size());
			for(const RetargetSemanticMapping& authored : profile.semanticMappings) {
				MappingWork mapping;
				mapping.mappingId = authored.mappingId;
				mapping.parentMappingId = authored.parentMappingId;
				mapping.basisId = authored.basisId;
				mapping.fallbackRuleId = authored.fallbackRuleId;
				mapping.sourceJointId = authored.sourceJointId;
				mapping.targetJointId = authored.targetJointId;
				mapping.sourceRole = authored.sourceSemantic;
				mapping.targetRole = authored.targetSemantic;
				mapping.kind = RetargetMappingKind::Semantic;
				mapping.required = authored.required;
				ResolveSemanticSide(sourceSkeleton, authored.sourceSemantic,
					authored.sourceJointId, true, mapping, result);
				ResolveSemanticSide(targetSkeleton, authored.targetSemantic,
					authored.targetJointId, false, mapping, result);
				mappings.push_back(std::move(mapping));
			}
			for(const RetargetExplicitJointMapping& authored : profile.explicitMappings) {
				MappingWork mapping;
				mapping.mappingId = authored.mappingId;
				mapping.parentMappingId = authored.parentMappingId;
				mapping.basisId = authored.basisId;
				mapping.fallbackRuleId = authored.fallbackRuleId;
				mapping.sourceJointId = authored.sourceJointId;
				mapping.targetJointId = authored.targetJointId;
				mapping.kind = RetargetMappingKind::Explicit;
				mapping.required = authored.required;
				ResolveExplicitSide(sourceSkeleton, authored.sourceJointId, true,
					mapping, result);
				ResolveExplicitSide(targetSkeleton, authored.targetJointId, false,
					mapping, result);
				mappings.push_back(std::move(mapping));
			}

			result.metrics.semanticMappingCount = static_cast<std::uint32_t>(
				profile.semanticMappings.size());
			result.metrics.explicitMappingCount = static_cast<std::uint32_t>(
				profile.explicitMappings.size());
			for(const MappingWork& mapping : mappings) {
				if(mapping.required) {
					++result.metrics.requiredMappingCount;
					if(mapping.sourceResolved && mapping.targetResolved) {
						++result.metrics.resolvedRequiredMappingCount;
					}
				} else {
					++result.metrics.optionalMappingCount;
					if(mapping.sourceResolved && mapping.targetResolved) {
						++result.metrics.resolvedOptionalMappingCount;
					}
				}
			}

			std::vector<std::uint64_t> mappingIds;
			mappingIds.reserve(mappings.size());
			for(const MappingWork& mapping : mappings) {
				mappingIds.push_back(mapping.mappingId);
			}
			std::sort(mappingIds.begin(), mappingIds.end());
			for(std::size_t index = 1U; index < mappingIds.size(); ++index) {
				if(mappingIds[index] == 0U || mappingIds[index] == mappingIds[index - 1U]) {
					AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
						RetargetDiagnosticSeverity::Error, mappingIds[index], 0U, 0U,
						{}, {}, "Mapping IDs must be non-zero and unique across semantic and explicit mappings.");
				}
			}
			if(!mappingIds.empty() && mappingIds.front() == 0U) {
				AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Mapping IDs must be non-zero and unique across semantic and explicit mappings.");
			}

			std::vector<std::uint64_t> resolvedSourceIds;
			std::vector<std::uint64_t> resolvedTargetIds;
			for(const MappingWork& mapping : mappings) {
				if(mapping.sourceResolved && mapping.targetResolved) {
					resolvedSourceIds.push_back(mapping.sourceJointId);
					resolvedTargetIds.push_back(mapping.targetJointId);
				}
			}
			std::sort(resolvedSourceIds.begin(), resolvedSourceIds.end());
			std::sort(resolvedTargetIds.begin(), resolvedTargetIds.end());
			for(const auto* ids : {&resolvedSourceIds, &resolvedTargetIds}) {
				for(std::size_t index = 1U; index < ids->size(); ++index) {
					if((*ids)[index] == (*ids)[index - 1U]) {
						AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
							RetargetDiagnosticSeverity::Error, 0U,
							ids == &resolvedSourceIds ? (*ids)[index] : 0U,
							ids == &resolvedTargetIds ? (*ids)[index] : 0U,
							{}, {}, "A joint is mapped more than once in the same retarget profile.");
					}
				}
			}

			// Every mapping parent chain is walked independently. The hard mapping
			// count bounds the walk even for deliberately cyclic authored input.
			std::vector<std::uint64_t> cyclicMappings;
			for(const MappingWork& mapping : mappings) {
				std::vector<std::uint64_t> path;
				const MappingWork* current = &mapping;
				while(current != nullptr && current->parentMappingId != 0U
					&& path.size() <= mappings.size()) {
					if(std::find(path.begin(), path.end(), current->mappingId) != path.end()) {
						cyclicMappings.insert(cyclicMappings.end(), path.begin(), path.end());
						break;
					}
					path.push_back(current->mappingId);
					current = FindMapping(mappings, current->parentMappingId);
				}
				if(current == nullptr && !path.empty()) {
					AddDiagnostic(result, RetargetDiagnosticCode::MissingMapping,
						RetargetDiagnosticSeverity::Error, mapping.mappingId,
						mapping.sourceJointId, mapping.targetJointId,
						mapping.sourceRole, mapping.targetRole,
						"Mapping parent ID does not name another mapping.");
				}
			}
			std::sort(cyclicMappings.begin(), cyclicMappings.end());
			cyclicMappings.erase(std::unique(cyclicMappings.begin(), cyclicMappings.end()),
				cyclicMappings.end());
			for(const std::uint64_t mappingId : cyclicMappings) {
				AddDiagnostic(result, RetargetDiagnosticCode::CyclicMapping,
					RetargetDiagnosticSeverity::Error, mappingId, 0U, 0U, {}, {},
					"Mapping parent chain contains a direct or indirect cycle.");
			}

			for(const MappingWork& mapping : mappings) {
				if(mapping.parentMappingId == 0U || !mapping.sourceResolved
					|| !mapping.targetResolved) {
					continue;
				}
				const MappingWork* parent = FindMapping(mappings, mapping.parentMappingId);
				if(parent != nullptr && parent->sourceResolved && parent->targetResolved
					&& (!IsDescendant(sourceSkeleton, mapping.sourceJointIndex,
						parent->sourceJointIndex)
						|| !IsDescendant(targetSkeleton, mapping.targetJointIndex,
							parent->targetJointIndex))) {
					AddDiagnostic(result, RetargetDiagnosticCode::InvalidHierarchy,
						RetargetDiagnosticSeverity::Error, mapping.mappingId,
						mapping.sourceJointId, mapping.targetJointId,
						mapping.sourceRole, mapping.targetRole,
						"Source and target mapping parent relationships are incompatible.");
				}
			}

			std::vector<std::uint64_t> basisIds;
			for(const RetargetOrientationBasis& basis : profile.orientationBases) {
				basisIds.push_back(basis.basisId);
				if(basis.basisId == 0U || !FindJointIndex(sourceSkeleton, basis.sourceJointId)
					|| !FindJointIndex(targetSkeleton, basis.targetJointId)
					|| !IsNormalized(basis.sourceBasis, 0.01F)
					|| !IsNormalized(basis.targetBasis, 0.01F)) {
					AddDiagnostic(result, RetargetDiagnosticCode::InvalidBasis,
						RetargetDiagnosticSeverity::Error, basis.basisId,
						basis.sourceJointId, basis.targetJointId, {}, {},
						"Orientation basis must name existing joints and contain normalized finite rotations.");
				}
			}
			std::sort(basisIds.begin(), basisIds.end());
			if(std::adjacent_find(basisIds.begin(), basisIds.end()) != basisIds.end()) {
				AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Orientation basis IDs must be unique.");
			}
			for(const MappingWork& mapping : mappings) {
				if(mapping.basisId == 0U) {
					continue;
				}
				const RetargetOrientationBasis* basis = FindBasis(profile, mapping.basisId);
				if(basis == nullptr || (mapping.sourceResolved
					&& basis->sourceJointId != mapping.sourceJointId)
					|| (mapping.targetResolved
						&& basis->targetJointId != mapping.targetJointId)) {
					AddDiagnostic(result, RetargetDiagnosticCode::InvalidBasis,
						RetargetDiagnosticSeverity::Error, mapping.mappingId,
						mapping.sourceJointId, mapping.targetJointId,
						mapping.sourceRole, mapping.targetRole,
						"Mapping basis is missing or belongs to different joints.");
				}
			}

			result.metrics.chainCount = static_cast<std::uint32_t>(profile.chainMappings.size());
			std::vector<std::uint64_t> chainIds;
			for(const RetargetChainMapping& chain : profile.chainMappings) {
				chainIds.push_back(chain.chainId);
				if(chain.chainId == 0U || (chain.required
					&& (chain.sourceJointIds.empty() || chain.targetJointIds.empty()))) {
					AddDiagnostic(result, RetargetDiagnosticCode::MissingMapping,
						RetargetDiagnosticSeverity::Error, chain.chainId, 0U, 0U,
						chain.semantic, chain.semantic,
						"Required chain mappings need non-empty source and target anchors.");
					continue;
				}
				for(const bool source : {true, false}) {
					const SkeletonAsset& skeleton = source ? sourceSkeleton : targetSkeleton;
					const std::vector<std::uint64_t>& ids = source
						? chain.sourceJointIds : chain.targetJointIds;
					std::optional<std::uint32_t> prior;
					std::vector<std::uint64_t> seenJointIds;
					seenJointIds.reserve(ids.size());
					for(const std::uint64_t jointId : ids) {
						if(std::find(seenJointIds.begin(), seenJointIds.end(), jointId)
							!= seenJointIds.end()) {
							AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
								RetargetDiagnosticSeverity::Error, chain.chainId,
								source ? jointId : 0U, source ? 0U : jointId,
								chain.semantic, chain.semantic,
								"Chain anchors must not repeat a joint stable ID.");
						}
						seenJointIds.push_back(jointId);
						const std::optional<std::uint32_t> index = FindJointIndex(skeleton, jointId);
						if(!index.has_value()) {
							AddDiagnostic(result, RetargetDiagnosticCode::MissingMapping,
								chain.required ? RetargetDiagnosticSeverity::Error
									: RetargetDiagnosticSeverity::Warning,
								chain.chainId, source ? jointId : 0U,
								source ? 0U : jointId, chain.semantic,
								chain.semantic, "Chain anchor joint is missing from its skeleton.");
							continue;
						}
						if(prior.has_value() && !IsDescendant(skeleton, *index, *prior)) {
							AddDiagnostic(result, RetargetDiagnosticCode::InvalidHierarchy,
								RetargetDiagnosticSeverity::Error, chain.chainId,
								source ? jointId : 0U, source ? 0U : jointId,
								chain.semantic, chain.semantic,
								"Chain anchors must follow root-to-effector hierarchy order.");
						}
						prior = index;
					}
				}
			}
			std::sort(chainIds.begin(), chainIds.end());
			if(std::adjacent_find(chainIds.begin(), chainIds.end()) != chainIds.end()) {
				AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Retarget chain IDs must be unique.");
			}

			result.metrics.contactCount = static_cast<std::uint32_t>(profile.contacts.size());
			std::vector<std::uint64_t> contactIds;
			for(const RetargetContactRule& contact : profile.contacts) {
				contactIds.push_back(contact.contactId);
				result.metrics.maximumContactToleranceMeters = std::max(
					result.metrics.maximumContactToleranceMeters, contact.toleranceMeters);
				const bool sourceExists = FindJointIndex(sourceSkeleton, contact.sourceJointId).has_value();
				const bool targetExists = FindJointIndex(targetSkeleton, contact.targetJointId).has_value();
				const auto chain = std::find_if(profile.chainMappings.begin(),
					profile.chainMappings.end(), [&contact](const RetargetChainMapping& candidate) {
						return candidate.chainId == contact.targetChainId;
					});
				const bool chainExists = contact.targetChainId == 0U
					|| (chain != profile.chainMappings.end()
						&& !chain->targetJointIds.empty()
						&& std::find(chain->targetJointIds.begin(),
							chain->targetJointIds.end(), contact.targetJointId)
							!= chain->targetJointIds.end());
				if(contact.contactId == 0U || !sourceExists || !targetExists || !chainExists
					|| !std::isfinite(contact.toleranceMeters)
					|| contact.toleranceMeters <= 0.0F
					|| contact.toleranceMeters > kRetargetDefaultContactToleranceMeters) {
					AddDiagnostic(result, RetargetDiagnosticCode::InvalidContact,
						RetargetDiagnosticSeverity::Error, contact.contactId,
						contact.sourceJointId, contact.targetJointId, {}, {},
						"Contact rule must name existing joints/chain and cannot loosen the 20-mm contract.",
						contact.toleranceMeters, kRetargetDefaultContactToleranceMeters);
				}
			}
			std::sort(contactIds.begin(), contactIds.end());
			if(std::adjacent_find(contactIds.begin(), contactIds.end()) != contactIds.end()) {
				AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Retarget contact IDs must be unique.");
			}

			std::vector<std::uint64_t> twistIds;
			for(const RetargetTwistRule& twist : profile.twistRules) {
				twistIds.push_back(twist.twistId);
				result.metrics.twistJointCount += static_cast<std::uint32_t>(
					twist.targetJointIds.size());
				const std::optional<std::uint32_t> source = FindJointIndex(
					sourceSkeleton, twist.sourceJointId);
				const std::optional<std::uint32_t> child = FindJointIndex(
					sourceSkeleton, twist.sourceChildJointId);
				float weightSum = 0.0F;
				bool validTargets = !twist.targetJointIds.empty()
					&& twist.targetJointIds.size() == twist.targetWeights.size();
				std::optional<std::uint32_t> priorTarget;
				for(std::size_t index = 0U; index < twist.targetJointIds.size(); ++index) {
					const std::optional<std::uint32_t> target = FindJointIndex(
						targetSkeleton, twist.targetJointIds[index]);
					if(index >= twist.targetWeights.size()) {
						validTargets = false;
						continue;
					}
					validTargets = validTargets && target.has_value()
						&& std::isfinite(twist.targetWeights[index])
						&& twist.targetWeights[index] >= 0.0F;
					if(target.has_value() && priorTarget.has_value()) {
						validTargets = validTargets
							&& IsDescendant(targetSkeleton, *target, *priorTarget);
					}
					priorTarget = target;
					weightSum += twist.targetWeights[index];
				}
				if(twist.twistId == 0U || !source.has_value() || !child.has_value()
					|| !IsDescendant(sourceSkeleton, *child, *source)
					|| !validTargets || !IsFinite(twist.sourceTwistAxis)
					|| std::abs(weightSum - 1.0F) > 0.001F) {
					AddDiagnostic(result, RetargetDiagnosticCode::InvalidTwist,
						RetargetDiagnosticSeverity::Error, twist.twistId,
						twist.sourceJointId, twist.targetJointIds.empty()
							? 0U : twist.targetJointIds.front(), {}, {},
						"Twist rule requires an ordered source/target chain and non-negative unit-sum weights.",
						weightSum, 1.0F);
				}
			}
			std::sort(twistIds.begin(), twistIds.end());
			if(std::adjacent_find(twistIds.begin(), twistIds.end()) != twistIds.end()) {
				AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Retarget twist IDs must be unique.");
			}

			result.metrics.fallbackCount = static_cast<std::uint32_t>(profile.fallbackRules.size());
			std::vector<std::uint64_t> fallbackIds;
			for(const RetargetFallbackRule& fallback : profile.fallbackRules) {
				fallbackIds.push_back(fallback.fallbackRuleId);
				if(fallback.fallbackRuleId == 0U
					|| FindMapping(mappings, fallback.sourceMappingId) == nullptr) {
					AddDiagnostic(result, RetargetDiagnosticCode::MissingFallback,
						RetargetDiagnosticSeverity::Error, fallback.fallbackRuleId,
						0U, 0U, {}, {},
						"Fallback rule must name the optional mapping it serves.");
				}
				for(const std::uint64_t targetJointId : fallback.targetJointIds) {
					if(!FindJointIndex(targetSkeleton, targetJointId).has_value()) {
						AddDiagnostic(result, RetargetDiagnosticCode::MissingFallback,
							RetargetDiagnosticSeverity::Error, fallback.fallbackRuleId,
							0U, targetJointId, {}, {},
							"Fallback target joint is absent from the target skeleton.");
					}
				}
			}
			std::sort(fallbackIds.begin(), fallbackIds.end());
			if(std::adjacent_find(fallbackIds.begin(), fallbackIds.end()) != fallbackIds.end()) {
				AddDiagnostic(result, RetargetDiagnosticCode::DuplicateMapping,
					RetargetDiagnosticSeverity::Error, 0U, 0U, 0U, {}, {},
					"Fallback rule IDs must be unique.");
			}
			for(const RetargetFallbackRule& fallback : profile.fallbackRules) {
				std::vector<std::uint64_t> path;
				const RetargetFallbackRule* current = &fallback;
				while(current != nullptr && current->nextFallbackRuleId != 0U
					&& path.size() <= profile.fallbackRules.size()) {
					if(std::find(path.begin(), path.end(), current->fallbackRuleId)
						!= path.end()) {
						AddDiagnostic(result, RetargetDiagnosticCode::CyclicMapping,
							RetargetDiagnosticSeverity::Error, fallback.fallbackRuleId,
							0U, 0U, {}, {},
							"Fallback chain contains a direct or indirect cycle.");
						break;
					}
					path.push_back(current->fallbackRuleId);
					current = FindFallback(profile, current->nextFallbackRuleId);
				}
				if(current == nullptr && !path.empty()) {
					AddDiagnostic(result, RetargetDiagnosticCode::MissingFallback,
						RetargetDiagnosticSeverity::Error, fallback.fallbackRuleId,
						0U, 0U, {}, {},
						"Fallback next ID does not name another fallback rule.");
				}
			}

			for(const MappingWork& mapping : mappings) {
				if(mapping.sourceResolved && mapping.targetResolved) {
					continue;
				}
				if(!mapping.required && mapping.fallbackRuleId != 0U
					&& FindFallback(profile, mapping.fallbackRuleId) != nullptr) {
					AddDiagnostic(result, RetargetDiagnosticCode::ReducedFidelity,
						RetargetDiagnosticSeverity::Warning, mapping.mappingId,
						mapping.sourceJointId, mapping.targetJointId,
						mapping.sourceRole, mapping.targetRole,
						"Optional mapping is unavailable; the declared bounded fallback will be used.");
				} else if(!mapping.required) {
					AddDiagnostic(result, RetargetDiagnosticCode::MissingFallback,
						RetargetDiagnosticSeverity::Error, mapping.mappingId,
						mapping.sourceJointId, mapping.targetJointId,
						mapping.sourceRole, mapping.targetRole,
						"An unavailable optional mapping requires a declared fallback rule.");
				}
			}

			const std::optional<std::uint32_t> sourceRoot = FindJointIndex(
				sourceSkeleton, profile.root.sourceRootJointId);
			const std::optional<std::uint32_t> targetRoot = FindJointIndex(
				targetSkeleton, profile.root.targetRootJointId);
			if(!sourceRoot.has_value() || !targetRoot.has_value()) {
				AddDiagnostic(result, RetargetDiagnosticCode::MissingMapping,
					RetargetDiagnosticSeverity::Error, 0U,
					profile.root.sourceRootJointId, profile.root.targetRootJointId,
					"root", "root", "Root-motion rule must name existing source and target roots.");
			}

			const float sourceHeight = SkeletonHeight(sourceSkeleton);
			const float targetHeight = SkeletonHeight(targetSkeleton);
			result.metrics.sourceToTargetHeightRatio = sourceHeight > 0.0F
				&& targetHeight > 0.0F ? targetHeight / sourceHeight
				: profile.proportion.targetReferenceHeightMeters
					/ profile.proportion.sourceReferenceHeightMeters;
			result.metrics.requiredCoverage = result.metrics.requiredMappingCount == 0U
				? 1.0F
				: static_cast<float>(result.metrics.resolvedRequiredMappingCount)
					/ static_cast<float>(result.metrics.requiredMappingCount);
			result.metrics.optionalCoverage = result.metrics.optionalMappingCount == 0U
				? 1.0F
				: static_cast<float>(result.metrics.resolvedOptionalMappingCount)
					/ static_cast<float>(result.metrics.optionalMappingCount);
			result.metrics.qualityScore = std::clamp(
				result.metrics.requiredCoverage * 0.8F
					+ result.metrics.optionalCoverage * 0.2F,
				0.0F, 1.0F);

			std::sort(result.diagnostics.begin(), result.diagnostics.end(), DiagnosticLess);
			result.status = FinalStatus(result);
			result.publishable = result.status == AnimationStatus::Ok;
			if(!result.publishable || compiled == nullptr) {
				return result.status;
			}

			RetargetCompiledProfile staged;
			staged.profile = profile;
			staged.profile.runtimeGeneration = 0U;
			staged.metrics = result.metrics;
			for(const MappingWork& mapping : mappings) {
				if(!mapping.sourceResolved || !mapping.targetResolved) {
					continue;
				}
				RetargetCompiledJointMapping resolved;
				resolved.mappingId = mapping.mappingId;
				resolved.parentMappingId = mapping.parentMappingId;
				resolved.fallbackRuleId = mapping.fallbackRuleId;
				resolved.sourceJointIndex = mapping.sourceJointIndex;
				resolved.targetJointIndex = mapping.targetJointIndex;
				resolved.kind = mapping.kind;
				resolved.required = mapping.required;
				if(const RetargetOrientationBasis* basis = FindBasis(profile, mapping.basisId)) {
					resolved.sourceBasis = basis->sourceBasis;
					resolved.targetBasis = basis->targetBasis;
				}
				staged.mappings.push_back(resolved);
			}
			std::sort(staged.mappings.begin(), staged.mappings.end(),
				[](const RetargetCompiledJointMapping& left,
					const RetargetCompiledJointMapping& right) {
					return std::tie(left.targetJointIndex, left.mappingId)
						< std::tie(right.targetJointIndex, right.mappingId);
				});
			for(const RetargetChainMapping& chain : profile.chainMappings) {
				RetargetCompiledChain resolved;
				resolved.chainId = chain.chainId;
				resolved.translationScaleMode = chain.translationScaleMode;
				resolved.authoredTranslationScale = chain.authoredTranslationScale;
				resolved.required = chain.required;
				for(const std::uint64_t id : chain.sourceJointIds) {
					if(const std::optional<std::uint32_t> index = FindJointIndex(sourceSkeleton, id)) {
						resolved.sourceJointIndices.push_back(*index);
					}
				}
				for(const std::uint64_t id : chain.targetJointIds) {
					if(const std::optional<std::uint32_t> index = FindJointIndex(targetSkeleton, id)) {
						resolved.targetJointIndices.push_back(*index);
					}
				}
				staged.chains.push_back(std::move(resolved));
			}
			std::sort(staged.chains.begin(), staged.chains.end(),
				[](const RetargetCompiledChain& left, const RetargetCompiledChain& right) {
					return left.chainId < right.chainId;
				});
			std::vector<std::uint8_t> bytes;
			if(const AnimationStatus serialization = SerializeRetargetProfileAsset(
				staged.profile, bytes); serialization != AnimationStatus::Ok) {
				return serialization;
			}
			staged.stableHash = HashCompiledBytes(bytes);
			*compiled = std::move(staged);
			return AnimationStatus::Ok;
	}
	}

	AnimationStatus ValidateRetargetProfile(
		const RetargetProfileAsset& profile,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		RetargetValidationResult& output) noexcept {
		try {
			RetargetValidationResult staged;
			const AnimationStatus status = RunValidation(profile, sourceSkeleton,
				targetSkeleton, staged, nullptr);
			output = std::move(staged);
			return status;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus CompileRetargetProfile(
		const RetargetProfileAsset& profile,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		RetargetValidationResult& validationOutput,
		RetargetCompiledProfile& compiledOutput) noexcept {
		try {
			RetargetValidationResult stagedValidation;
			RetargetCompiledProfile stagedCompiled;
			const AnimationStatus status = RunValidation(profile, sourceSkeleton,
				targetSkeleton, stagedValidation, &stagedCompiled);
			validationOutput = std::move(stagedValidation);
			if(status == AnimationStatus::Ok) {
				compiledOutput = std::move(stagedCompiled);
			}
			return status;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
