// AnimationDivergence.cpp - Bounded canonical first-difference localization.

#include "Animation/Determinism/AnimationDivergence.h"

#include <algorithm>
#include <new>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace TerraEngine::Animation {
	namespace {
		using SnapshotPointer = const AnimationRollbackSnapshot*;

		[[nodiscard]] auto SnapshotKey(const AnimationRollbackSnapshot& snapshot) noexcept
		{
			// Tick is the primary time axis. Slot and generation make simultaneous
			// animators deterministic without relying on capture arrival order.
			return std::tuple{
				snapshot.tick,
				snapshot.animator.slotIndex,
				snapshot.animator.generation};
		}

		[[nodiscard]] bool SnapshotPointerLess(
			SnapshotPointer left,
			SnapshotPointer right) noexcept
		{
			return SnapshotKey(*left) < SnapshotKey(*right);
		}

		[[nodiscard]] AnimationDivergencePoint Point(
			SnapshotPointer expected,
			SnapshotPointer actual,
			AnimationDivergenceField field,
			AnimationDeterministicStateKind category,
			std::uint64_t stableId,
			std::uint32_t ordinal = 0U) noexcept
		{
			const AnimationRollbackSnapshot* present = expected != nullptr ? expected : actual;
			AnimationDivergencePoint point;
			point.animator = present->animator;
			point.tick = present->tick;
			point.field = field;
			point.category = category;
			point.stableId = stableId;
			point.ordinal = ordinal;
			point.expectedChecksum = expected != nullptr ? expected->poseChecksum : 0U;
			point.actualChecksum = actual != nullptr ? actual->poseChecksum : 0U;
			point.expectedFingerprint = expected != nullptr
				? expected->fingerprint
				: AnimationDeterministicFingerprint{};
			point.actualFingerprint = actual != nullptr
				? actual->fingerprint
				: AnimationDeterministicFingerprint{};
			point.expectedPresent = expected != nullptr;
			point.actualPresent = actual != nullptr;
			point.found = true;
			return point;
		}

		template<typename Value, typename Key>
		[[nodiscard]] bool FindRecordDifference(
			const std::vector<Value>& expected,
			const std::vector<Value>& actual,
			Key key,
			const Value*& expectedValue,
			const Value*& actualValue) noexcept
		{
			std::size_t expectedIndex = 0U;
			std::size_t actualIndex = 0U;
			while(expectedIndex < expected.size() && actualIndex < actual.size()) {
				const auto expectedKey = key(expected[expectedIndex]);
				const auto actualKey = key(actual[actualIndex]);
				if(expectedKey < actualKey) {
					expectedValue = &expected[expectedIndex];
					actualValue = nullptr;
					return true;
				}
				if(actualKey < expectedKey) {
					expectedValue = nullptr;
					actualValue = &actual[actualIndex];
					return true;
				}
				if(expected[expectedIndex] != actual[actualIndex]) {
					expectedValue = &expected[expectedIndex];
					actualValue = &actual[actualIndex];
					return true;
				}
				++expectedIndex;
				++actualIndex;
			}
			if(expectedIndex < expected.size()) {
				expectedValue = &expected[expectedIndex];
				actualValue = nullptr;
				return true;
			}
			if(actualIndex < actual.size()) {
				expectedValue = nullptr;
				actualValue = &actual[actualIndex];
				return true;
			}
			return false;
		}

		template<typename Value, typename Key, typename StableId>
		[[nodiscard]] std::optional<AnimationDivergencePoint> VectorPoint(
			const AnimationRollbackSnapshot& expectedSnapshot,
			const AnimationRollbackSnapshot& actualSnapshot,
			const std::vector<Value>& expected,
			const std::vector<Value>& actual,
			Key key,
			StableId stableId,
			AnimationDivergenceField field,
			AnimationDeterministicStateKind category) noexcept
		{
			const Value* expectedValue = nullptr;
			const Value* actualValue = nullptr;
			if(!FindRecordDifference(
				expected, actual, key, expectedValue, actualValue)) {
				return std::nullopt;
			}
			const Value& present = expectedValue != nullptr ? *expectedValue : *actualValue;
			return Point(
				&expectedSnapshot,
				&actualSnapshot,
				field,
				category,
				stableId(present));
		}

		[[nodiscard]] AnimationDivergenceField FieldForGeneric(
			AnimationDeterministicStateKind kind) noexcept
		{
			switch(kind) {
			case AnimationDeterministicStateKind::Event:
				return AnimationDivergenceField::Event;
			case AnimationDeterministicStateKind::RootMotion:
				return AnimationDivergenceField::RootMotion;
			case AnimationDeterministicStateKind::MotionMatch:
				return AnimationDivergenceField::MotionMatch;
			case AnimationDeterministicStateKind::Ik:
			case AnimationDeterministicStateKind::Retarget:
			case AnimationDeterministicStateKind::Morph:
			case AnimationDeterministicStateKind::Ragdoll:
			case AnimationDeterministicStateKind::Attachment:
			case AnimationDeterministicStateKind::Physics:
				return AnimationDivergenceField::PoseElement;
			default:
				return AnimationDivergenceField::GraphElement;
			}
		}

		[[nodiscard]] std::optional<AnimationDivergencePoint> GenerationPoint(
			const AnimationRollbackSnapshot& expected,
			const AnimationRollbackSnapshot& actual) noexcept
		{
			struct GenerationPair {
				AnimationGeneration expected = 0U;
				AnimationGeneration actual = 0U;
			};
			const std::array pairs{
				GenerationPair{expected.publicationGeneration, actual.publicationGeneration},
				GenerationPair{expected.generations.assetGeneration, actual.generations.assetGeneration},
				GenerationPair{expected.generations.programGeneration, actual.generations.programGeneration},
				GenerationPair{expected.generations.objectGeneration, actual.generations.objectGeneration},
				GenerationPair{expected.generations.componentGeneration, actual.generations.componentGeneration},
				GenerationPair{expected.generations.sceneGeneration, actual.generations.sceneGeneration},
				GenerationPair{expected.generations.physicsWorldGeneration,
					actual.generations.physicsWorldGeneration},
				GenerationPair{expected.generations.historyGeneration,
					actual.generations.historyGeneration}};
			for(const GenerationPair pair : pairs) {
				if(pair.expected == pair.actual) {
					continue;
				}
				AnimationDivergencePoint point = Point(
					&expected,
					&actual,
					AnimationDivergenceField::AssetGeneration,
					AnimationDeterministicStateKind::Controller,
					0U);
				point.expectedGeneration = pair.expected;
				point.actualGeneration = pair.actual;
				return point;
			}
			return std::nullopt;
		}

		[[nodiscard]] std::optional<AnimationDivergencePoint> SnapshotDifference(
			const AnimationRollbackSnapshot& expected,
			const AnimationRollbackSnapshot& actual) noexcept
		{
			if(ValidateRollbackSnapshot(expected) != AnimationStatus::Ok
				|| ValidateRollbackSnapshot(actual) != AnimationStatus::Ok) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::SnapshotIntegrity,
					AnimationDeterministicStateKind::Controller,
					0U);
			}
			if(expected.fingerprint != actual.fingerprint) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::AssetFingerprint,
					AnimationDeterministicStateKind::Controller,
					0U);
			}
			if(const auto generation = GenerationPoint(expected, actual)) {
				return generation;
			}

			if(const auto difference = VectorPoint(
				expected, actual, expected.controllers, actual.controllers,
				[](const auto& value) { return value.controllerInstanceId; },
				[](const auto& value) { return value.controllerInstanceId; },
				AnimationDivergenceField::GraphElement,
				AnimationDeterministicStateKind::Controller)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.clips, actual.clips,
				[](const auto& value) { return value.clipInstanceId; },
				[](const auto& value) { return value.clipInstanceId; },
				AnimationDivergenceField::GraphElement,
				AnimationDeterministicStateKind::Clip)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.transitions, actual.transitions,
				[](const auto& value) { return value.transitionStableId; },
				[](const auto& value) { return value.transitionStableId; },
				AnimationDivergenceField::GraphElement,
				AnimationDeterministicStateKind::Transition)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.layers, actual.layers,
				[](const auto& value) { return value.layerStableId; },
				[](const auto& value) { return value.layerStableId; },
				AnimationDivergenceField::GraphElement,
				AnimationDeterministicStateKind::Layer)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.parameters, actual.parameters,
				[](const auto& value) { return value.parameterId; },
				[](const auto& value) { return value.parameterId; },
				AnimationDivergenceField::GraphElement,
				AnimationDeterministicStateKind::Parameter)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.triggers, actual.triggers,
				[](const auto& value) {
					return std::tuple{value.parameterId, value.sequenceId};
				},
				[](const auto& value) { return value.parameterId; },
				AnimationDivergenceField::GraphElement,
				AnimationDeterministicStateKind::Trigger)) {
				return difference;
			}

			const AnimationDeterministicStateRecord* expectedGeneric = nullptr;
			const AnimationDeterministicStateRecord* actualGeneric = nullptr;
			if(FindRecordDifference(
				expected.state,
				actual.state,
				[](const AnimationDeterministicStateRecord& value) {
					return std::tuple{value.kind, value.stableId, value.ordinal};
				},
				expectedGeneric,
				actualGeneric)) {
				const AnimationDeterministicStateRecord& present = expectedGeneric != nullptr
					? *expectedGeneric
					: *actualGeneric;
				return Point(
					&expected,
					&actual,
					FieldForGeneric(present.kind),
					present.kind,
					present.stableId,
					present.ordinal);
			}

			if(const auto difference = VectorPoint(
				expected, actual, expected.events, actual.events,
				[](const auto& value) {
					return std::tuple{
						value.streamId,
						value.stateInstanceId,
						value.clipAssetId,
						value.eventId,
						value.sourceTick,
						value.loopCount};
				},
				[](const auto& value) { return value.eventId; },
				AnimationDivergenceField::Event,
				AnimationDeterministicStateKind::Event)) {
				return difference;
			}
			if(expected.eventWatermark != actual.eventWatermark) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::Event,
					AnimationDeterministicStateKind::Event,
					expected.eventWatermark.streamId != 0U
						? expected.eventWatermark.streamId
						: actual.eventWatermark.streamId);
			}

			if(const auto difference = VectorPoint(
				expected, actual, expected.rootMotionStates, actual.rootMotionStates,
				[](const auto& value) { return value.accumulationId; },
				[](const auto& value) { return value.accumulationId; },
				AnimationDivergenceField::RootMotion,
				AnimationDeterministicStateKind::RootMotion)) {
				return difference;
			}
			if(expected.rootMotion != actual.rootMotion) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::RootMotion,
					AnimationDeterministicStateKind::RootMotion,
					expected.rootMotion.correlationId != 0U
						? expected.rootMotion.correlationId
						: actual.rootMotion.correlationId);
			}

			if(const auto difference = VectorPoint(
				expected, actual, expected.ikStates, actual.ikStates,
				[](const auto& value) { return std::tuple{value.stableNodeId, value.goalId}; },
				[](const auto& value) { return value.stableNodeId; },
				AnimationDivergenceField::PoseElement,
				AnimationDeterministicStateKind::Ik)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.retargetStates, actual.retargetStates,
				[](const auto& value) { return value.stableNodeId; },
				[](const auto& value) { return value.stableNodeId; },
				AnimationDivergenceField::PoseElement,
				AnimationDeterministicStateKind::Retarget)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.motionMatchStates, actual.motionMatchStates,
				[](const auto& value) { return value.stableNodeId; },
				[](const auto& value) { return value.stableNodeId; },
				AnimationDivergenceField::MotionMatch,
				AnimationDeterministicStateKind::MotionMatch)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.morphStates, actual.morphStates,
				[](const auto& value) { return std::tuple{value.targetStableId, value.sourceStableId}; },
				[](const auto& value) { return value.targetStableId; },
				AnimationDivergenceField::PoseElement,
				AnimationDeterministicStateKind::Morph)) {
				return difference;
			}
			if(const auto difference = VectorPoint(
				expected, actual, expected.ragdollStates, actual.ragdollStates,
				[](const auto& value) {
					return std::tuple{value.ragdollInstanceId, value.bodyId};
				},
				[](const auto& value) { return value.stableJointId; },
				AnimationDivergenceField::PoseElement,
				AnimationDeterministicStateKind::Ragdoll)) {
				return difference;
			}

			if(expected.poseChecksum != actual.poseChecksum
				&& expected.stateChecksum == actual.stateChecksum) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::PoseChecksum,
					AnimationDeterministicStateKind::Controller,
					0U);
			}
			if(expected.stateChecksum != actual.stateChecksum) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::SnapshotIntegrity,
					AnimationDeterministicStateKind::Controller,
					0U);
			}
			if(expected.stateChecksum != ComputeDeterministicStateChecksum(expected)
				|| actual.stateChecksum != ComputeDeterministicStateChecksum(actual)
				|| expected.poseChecksum != ComputeDeterministicSnapshotChecksum(expected)
				|| actual.poseChecksum != ComputeDeterministicSnapshotChecksum(actual)) {
				return Point(
					&expected,
					&actual,
					AnimationDivergenceField::SnapshotIntegrity,
					AnimationDeterministicStateKind::Controller,
					0U);
			}
			return std::nullopt;
		}
	}

	AnimationDivergenceAnalyzer::AnimationDivergenceAnalyzer(
		AnimationDivergenceConfiguration configuration) noexcept
		: configuration_(configuration)
	{
		configuration_.maximumSnapshots = std::clamp(
			configuration_.maximumSnapshots,
			std::size_t{1U},
			kAnimationDivergenceMaximumSnapshots);
	}

	AnimationStatus AnimationDivergenceAnalyzer::Analyze(
		std::span<const AnimationRollbackSnapshot> expected,
		std::span<const AnimationRollbackSnapshot> actual,
		AnimationDivergenceResult& output) const noexcept
	{
		if(expected.size() > configuration_.maximumSnapshots
			|| actual.size() > configuration_.maximumSnapshots) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			std::vector<SnapshotPointer> expectedOrder;
			std::vector<SnapshotPointer> actualOrder;
			expectedOrder.reserve(expected.size());
			actualOrder.reserve(actual.size());
			for(const AnimationRollbackSnapshot& snapshot : expected) {
				expectedOrder.push_back(&snapshot);
			}
			for(const AnimationRollbackSnapshot& snapshot : actual) {
				actualOrder.push_back(&snapshot);
			}
			std::sort(expectedOrder.begin(), expectedOrder.end(), SnapshotPointerLess);
			std::sort(actualOrder.begin(), actualOrder.end(), SnapshotPointerLess);
			for(std::size_t index = 1U; index < expectedOrder.size(); ++index) {
				if(SnapshotKey(*expectedOrder[index - 1U]) == SnapshotKey(*expectedOrder[index])) {
					return AnimationStatus::InvalidArgument;
				}
			}
			for(std::size_t index = 1U; index < actualOrder.size(); ++index) {
				if(SnapshotKey(*actualOrder[index - 1U]) == SnapshotKey(*actualOrder[index])) {
					return AnimationStatus::InvalidArgument;
				}
			}

			AnimationDivergenceResult candidate;
			candidate.status = AnimationStatus::Ok;
			candidate.retentionLimit = configuration_.maximumSnapshots;
			std::size_t expectedIndex = 0U;
			std::size_t actualIndex = 0U;
			while(expectedIndex < expectedOrder.size() && actualIndex < actualOrder.size()) {
				const auto expectedKey = SnapshotKey(*expectedOrder[expectedIndex]);
				const auto actualKey = SnapshotKey(*actualOrder[actualIndex]);
				if(expectedKey < actualKey) {
					candidate.point = Point(
						expectedOrder[expectedIndex],
						nullptr,
						AnimationDivergenceField::MissingSnapshot,
						AnimationDeterministicStateKind::Controller,
						0U);
					output = std::move(candidate);
					return AnimationStatus::Ok;
				}
				if(actualKey < expectedKey) {
					candidate.point = Point(
						nullptr,
						actualOrder[actualIndex],
						AnimationDivergenceField::MissingSnapshot,
						AnimationDeterministicStateKind::Controller,
						0U);
					output = std::move(candidate);
					return AnimationStatus::Ok;
				}
				if(const auto difference = SnapshotDifference(
					*expectedOrder[expectedIndex], *actualOrder[actualIndex])) {
					candidate.point = *difference;
					output = std::move(candidate);
					return AnimationStatus::Ok;
				}
				++candidate.comparedSnapshots;
				++expectedIndex;
				++actualIndex;
			}
			if(expectedIndex < expectedOrder.size()) {
				candidate.point = Point(
					expectedOrder[expectedIndex],
					nullptr,
					AnimationDivergenceField::MissingSnapshot,
					AnimationDeterministicStateKind::Controller,
					0U);
			} else if(actualIndex < actualOrder.size()) {
				candidate.point = Point(
					nullptr,
					actualOrder[actualIndex],
					AnimationDivergenceField::MissingSnapshot,
					AnimationDeterministicStateKind::Controller,
					0U);
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationDivergencePoint AnimationDivergenceAnalyzer::FindFirstDifference(
		std::span<const AnimationRollbackSnapshot> expected,
		std::span<const AnimationRollbackSnapshot> actual) const noexcept
	{
		AnimationDivergenceResult result;
		return Analyze(expected, actual, result) == AnimationStatus::Ok
			? result.point
			: AnimationDivergencePoint{};
	}
}
