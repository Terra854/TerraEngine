// LocomotionAnimationBindingTests.cpp - checked animator parameter presentation.

#include "../TestHarness.h"

#include "Animation/AnimationSystem.h"
#include "../Animation/AnimationAuthorityTestHelpers.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionAnimationBindings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Locomotion;

		std::array<AnimationControllerParameter, kMotionParameterCount> MakeParameters()
		{
			return {{
				{101U, "Speed", AnimationControllerParameterType::Float, 0.0F},
				{102U, "DirectionX", AnimationControllerParameterType::Float, 0.0F},
				{103U, "DirectionZ", AnimationControllerParameterType::Float, 0.0F},
				{104U, "Grounded", AnimationControllerParameterType::Boolean, false},
				{105U, "Moving", AnimationControllerParameterType::Boolean, false}}};
		}

		MotionParameterNames MakeNames()
		{
			return {"Speed", "DirectionX", "DirectionZ", "Grounded", "Moving"};
		}

		LocomotionObservation MakeObservation()
		{
			LocomotionObservation observation{};
			observation.observedPlanarSpeed = 3.25;
			observation.observedLocalDirection = {0.6, 0.0, 0.8};
			observation.grounded = true;
			observation.motionHistoryAvailable = true;
			observation.observationDurationSeconds = 1.0 / 60.0;
			observation.physicalTick = 77U;
			return observation;
		}

		bool ExecuteAnimationPublicationFrame(AnimationSystem& system)
		{
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan result =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), "locomotion-animation-binding");
			return result.valid && result.completed;
		}

		bool Near(float left, float right)
		{
			return std::abs(left - right) <= 1.0e-5F;
		}
	}

	void RegisterLocomotionAnimationBindingTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.AnimationBinding.ResolvesAndPreparesFiveTypedObservedParameters",
			[](TestContext& context) {
				const auto parameters = MakeParameters();
				const AnimatorInstanceHandle animator{8U, 3U};
				const MotionParameterMapping mapping = ResolveMotionParameters(
					animator, 12U, MakeNames(), parameters);
				context.Expect(mapping.animator == animator && mapping.controllerGeneration == 12U
					&& mapping.error == MotionParameterError::None
					&& mapping.ids == std::array<std::uint32_t, kMotionParameterCount>{101U, 102U, 103U, 104U, 105U},
					"A complete authored mapping must resolve stable controller IDs with their checked controller generation.");

				const LocomotionObservation observation = MakeObservation();
				auto prepared =
					PrepareMotionParameters(mapping, observation, 700U, 9U);
				context.Expect(prepared.error == MotionParameterError::None
					&& prepared.count == kMotionParameterCount
					&& prepared.physicalTick == observation.physicalTick,
					"One immutable observation must prepare a complete five-field parameter batch stamped with its physical feedback tick.");
				if(prepared.count == kMotionParameterCount) {
					context.Expect(prepared.commands[0].parameterId == 101U
						&& prepared.commands[0].targetTick == 0
						&& prepared.commands[0].parameterType == AnimationParameterType::Float
						&& std::holds_alternative<float>(prepared.commands[0].value)
						&& Near(std::get<float>(prepared.commands[0].value), 3.25F)
						&& prepared.commands[1].parameterId == 102U
						&& Near(std::get<float>(prepared.commands[1].value), 0.6F)
						&& prepared.commands[2].parameterId == 103U
						&& Near(std::get<float>(prepared.commands[2].value), 0.8F)
						&& prepared.commands[3].parameterId == 104U
						&& prepared.commands[3].parameterType == AnimationParameterType::Boolean
						&& std::get<bool>(prepared.commands[3].value)
						&& prepared.commands[4].parameterId == 105U
						&& prepared.commands[4].parameterType == AnimationParameterType::Boolean
						&& std::get<bool>(prepared.commands[4].value),
						"The prepared batch must preserve observed speed, local direction, grounded state, moving state, and controller types.");

					AnimationSystem& animation = AnimationSystem::Instance();
					animation.Initialize();
					AnimatorInstanceDescriptor descriptor;
					descriptor.ownerObjectId[0U] = 206U;
					descriptor.componentGeneration = 1U;
					const AnimatorInstanceHandle liveAnimator = animation.CreateAnimatorInstance(descriptor);
					for(std::size_t index = 0U; index < prepared.count; ++index)
						prepared.commands[index].target = liveAnimator;
					std::array<AnimationCommand, 2U> invalidBatch{
						prepared.commands[0], prepared.commands[1]};
					invalidBatch[0].producerSequence = 900U;
					invalidBatch[0].correlationId = 900U;
					invalidBatch[1].producerSequence = 901U;
					invalidBatch[1].correlationId = 901U;
					invalidBatch[1].value = true;
					const AnimationCommandBatchReceipt rejectedBatch =
						animation.SubmitCommandBatch(invalidBatch);
					const bool everyRejectedReceiptIsTerminal = rejectedBatch.commands.size() == invalidBatch.size()
						&& std::all_of(rejectedBatch.commands.begin(), rejectedBatch.commands.end(), [](const auto& value) {
							return value.status == AnimationStatus::InvalidArgument;
						});
					context.Expect(rejectedBatch.status == AnimationStatus::InvalidArgument
						&& everyRejectedReceiptIsTerminal,
						"AnimationSystem must reject a malformed later value atomically without accepting the earlier valid parameter write.");
					const AnimationCommandBatchReceipt receipt = animation.SubmitCommandBatch(
						std::span<const AnimationCommand>(prepared.commands.data(), prepared.count));
					context.Expect(liveAnimator.IsValid() && receipt.status == AnimationStatus::Queued
						&& receipt.commands.size() == kMotionParameterCount,
					"The prepared parameters must enter AnimationSystem through its atomic native batch submission path.");
					context.Expect(ExecuteAnimationPublicationFrame(animation),
						"AnimationSystem must complete a publication frame for the prepared locomotion batch.");
					const PinnedAnimationAuthorityRow published =
						PinnedAnimationAuthorityRow::ForAnimator(animation, liveAnimator);
					context.Expect(published && published->snapshot
						&& published->snapshot->playback.parameters.size() == kMotionParameterCount,
						"The five prepared locomotion values must appear in the real immutable AnimationSystem parameter snapshot.");
					if(published && published->snapshot
						&& published->snapshot->playback.parameters.size() == kMotionParameterCount) {
						const auto& values = published->snapshot->playback.parameters;
						const auto find = [&values](std::uint32_t id) -> const AnimationParameterSnapshot* {
							const auto found = std::find_if(values.begin(), values.end(), [id](const auto& value) {
								return value.parameterId == id;
							});
							return found == values.end() ? nullptr : &*found;
						};
						const auto* speed = find(101U);
						const auto* directionX = find(102U);
						const auto* directionZ = find(103U);
						const auto* grounded = find(104U);
						const auto* moving = find(105U);
						context.Expect(speed && speed->type == AnimationParameterType::Float
							&& std::holds_alternative<float>(speed->value)
							&& Near(std::get<float>(speed->value), 3.25F)
							&& directionX && directionX->type == AnimationParameterType::Float
							&& std::holds_alternative<float>(directionX->value)
							&& Near(std::get<float>(directionX->value), 0.6F)
							&& directionZ && directionZ->type == AnimationParameterType::Float
							&& std::holds_alternative<float>(directionZ->value)
							&& Near(std::get<float>(directionZ->value), 0.8F)
							&& grounded && grounded->type == AnimationParameterType::Boolean
							&& std::holds_alternative<bool>(grounded->value)
							&& std::get<bool>(grounded->value)
							&& moving && moving->type == AnimationParameterType::Boolean
							&& std::holds_alternative<bool>(moving->value)
							&& std::get<bool>(moving->value),
							"The published parameter snapshot must contain all five observed values with exact stable IDs and types.");
					}
					if(liveAnimator.IsValid()) (void)animation.InvalidateAnimatorInstance(liveAnimator);
				}
			});

		registry.Add("Engine.Locomotion.AnimationBinding.RejectsWrongTypeDuplicateMissingAndUnsupportedIds",
			[](TestContext& context) {
				auto parameters = MakeParameters();
				MotionParameterNames names = MakeNames();
				parameters[0].type = AnimationControllerParameterType::Boolean;
				const auto wrongType = ResolveMotionParameters({2U, 1U}, 4U, names, parameters);
				context.Expect(wrongType.error == MotionParameterError::WrongType
					&& wrongType.ids == std::array<std::uint32_t, kMotionParameterCount>{},
					"A name bound to the wrong authored controller type must reject the complete mapping rather than partially map fields.");

				parameters = MakeParameters();
				names[1] = names[0];
				const auto duplicate = ResolveMotionParameters({2U, 1U}, 4U, names, parameters);
				context.Expect(duplicate.error == MotionParameterError::Duplicate,
					"Two locomotion outputs cannot target one controller parameter ID.");

				parameters = MakeParameters();
				names = MakeNames();
				names[4] = "NotPresent";
				const auto missing = ResolveMotionParameters({2U, 1U}, 4U, names, parameters);
				context.Expect(missing.error == MotionParameterError::Missing,
					"A missing required authored name must reject the complete mapping.");

				parameters = MakeParameters();
				parameters[2].stableId = static_cast<std::uint64_t>(
					std::numeric_limits<std::uint32_t>::max()) + 1U;
				const auto unsupported = ResolveMotionParameters({2U, 1U}, 4U, names, parameters);
				context.Expect(unsupported.error == MotionParameterError::UnsupportedId,
					"A stable controller ID outside the command ABI's uint32 range must fail closed.");
			});

		registry.Add("Engine.Locomotion.AnimationBinding.OptionalEmptyNamesAndInvalidObservationAreAtomic",
			[](TestContext& context) {
				auto parameters = MakeParameters();
				MotionParameterNames names = MakeNames();
				names[1].clear();
				names[2].clear();
				const auto optional = ResolveMotionParameters({3U, 1U}, 8U, names, parameters);
				context.Expect(optional.error == MotionParameterError::None,
					"Empty optional direction names must be omitted without disabling the remaining presentation mapping.");
				const auto preparedOptional = PrepareMotionParameters(
					optional, MakeObservation(), 701U, 1U);
				context.Expect(preparedOptional.error == MotionParameterError::None
					&& preparedOptional.count == 3U,
					"An optional mapping must prepare only the configured speed and boolean outputs.");

				const auto complete = ResolveMotionParameters({3U, 1U}, 8U, MakeNames(), parameters);
				auto invalid = MakeObservation();
				invalid.observedPlanarSpeed = std::numeric_limits<double>::infinity();
				const auto rejected = PrepareMotionParameters(complete, invalid, 701U, 2U);
				context.Expect(rejected.error == MotionParameterError::InvalidObservation
					&& rejected.count == 0U,
					"A non-finite observed field must reject the entire prepared parameter batch without partial commands.");
			});
	}
}
