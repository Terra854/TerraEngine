#include "PhysicsPreparationPlan.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <unordered_set>

namespace TerraEngine::Physics {
	namespace {
		bool LessOperation(
			const PhysicsPreparationOperation& left,
			const PhysicsPreparationOperation& right) {
			return left.objectId < right.objectId;
		}

		bool ValidateOperations(
			std::vector<PhysicsPreparationOperation>& operations,
			bool removals,
			bool allowNoDescriptor = false) {
			std::sort(operations.begin(), operations.end(), LessOperation);
			for(std::size_t index = 0U; index < operations.size(); ++index) {
				const PhysicsPreparationOperation& operation = operations[index];
				if(operation.objectId == TerraEngine::EMPTY_UUID
					|| operation.reasons == PhysicsAuthoringChangeReason::None
					|| (index > 0U && operations[index - 1U].objectId == operation.objectId)) {
					return false;
				}
				if((!allowNoDescriptor && removals != (operation.descriptorIndex == kInvalidPhysicsDescriptorIndex))
					|| (!removals && operation.objectGeneration == 0U)) {
					return false;
				}
			}
			return true;
		}

		void AppendWorkItems(
			PhysicsPreparationPlanData& data,
			PhysicsPreparationWorkKind kind,
			const std::vector<PhysicsPreparationOperation>& operations) {
			for(const PhysicsPreparationOperation& operation : operations) {
				PhysicsPreparationWorkItem item;
				item.kind = kind;
				item.stableOrdinal = data.workItems.size();
				item.operation = operation;
				data.workItems.push_back(item);
			}
		}

		void BuildRanges(PhysicsPreparationPlanData& data, std::size_t requestedRangeSize) {
			data.ranges.clear();
			const std::size_t rangeSize = std::max<std::size_t>(1U, requestedRangeSize);
			std::size_t begin = 0U;
			while(begin < data.workItems.size()) {
				const PhysicsPreparationWorkKind kind = data.workItems[begin].kind;
				std::size_t groupEnd = begin + 1U;
				while(groupEnd < data.workItems.size() && data.workItems[groupEnd].kind == kind) {
					++groupEnd;
				}
				for(std::size_t chunkBegin = begin; chunkBegin < groupEnd; chunkBegin += rangeSize) {
					data.ranges.push_back({
						kind,
						chunkBegin,
						std::min(rangeSize, groupEnd - chunkBegin),
						data.ranges.size()});
				}
				begin = groupEnd;
			}
		}
	}

	std::optional<PhysicsPreparationPlan> PhysicsPreparationPlan::Build(
		const PhysicsAuthoringGeneration& generation,
		PhysicsTick tick,
		PhysicsPreparationPlanOptions options) {
		if(!generation.IsValid()) return std::nullopt;

		PhysicsPreparationPlanData data;
		data.world = generation.Data().world;
		data.authoringGeneration = generation.Data().generation;
		data.tick = tick;
		for(const PhysicsAuthoringChange& change : generation.Data().changes) {
			PhysicsPreparationOperation operation{
				change.objectId,
				change.objectGeneration,
				change.descriptorIndex,
				change.reasons};
			if(HasAnyReason(change.reasons, PhysicsAuthoringChangeReason::Removed)) {
				data.removals.push_back(operation);
			} else if(HasAnyReason(change.reasons, PhysicsAuthoringChangeReason::Added)) {
				data.creates.push_back(operation);
			} else if(HasAnyReason(change.reasons,
				PhysicsAuthoringChangeReason::BodyComponent
					| PhysicsAuthoringChangeReason::ColliderComponent
					| PhysicsAuthoringChangeReason::ColliderAsset
					| PhysicsAuthoringChangeReason::MaterialAsset
					| PhysicsAuthoringChangeReason::HierarchyDependency
					| PhysicsAuthoringChangeReason::Settings
					| PhysicsAuthoringChangeReason::Lifecycle)) {
				// The current provider-neutral in-place request can update pose, velocity,
				// layer, mask, and enabled state, but cannot safely replace mass, damping,
				// material coefficients, shapes, or world-setting dependencies. Rebuild
				// those infrequent authoring edits instead of silently applying a subset.
				data.dependencyRebuilds.push_back(operation);
			} else {
				data.updates.push_back(operation);
			}
		}
		data.constraints = std::move(options.constraintOperations);
		data.characters = std::move(options.characterOperations);
		data.commands = std::move(options.commands);
		data.queries = std::move(options.queries);
		if(!ValidateOperations(data.constraints, false, true)
			|| !ValidateOperations(data.characters, false, true)) {
			return std::nullopt;
		}

		AppendWorkItems(data, PhysicsPreparationWorkKind::CreateBody, data.creates);
		AppendWorkItems(data, PhysicsPreparationWorkKind::RemoveBody, data.removals);
		AppendWorkItems(data, PhysicsPreparationWorkKind::UpdateBody, data.updates);
		AppendWorkItems(data, PhysicsPreparationWorkKind::RebuildDependency, data.dependencyRebuilds);
		AppendWorkItems(data, PhysicsPreparationWorkKind::Constraint, data.constraints);
		AppendWorkItems(data, PhysicsPreparationWorkKind::Character, data.characters);
		std::sort(data.commands.begin(), data.commands.end());
		data.commands.erase(std::unique(data.commands.begin(), data.commands.end()), data.commands.end());
		for(PhysicsCommandId command : data.commands) {
			PhysicsPreparationWorkItem item;
			item.kind = PhysicsPreparationWorkKind::Command;
			item.stableOrdinal = data.workItems.size();
			item.commandId = command;
			data.workItems.push_back(item);
		}
		std::sort(data.queries.begin(), data.queries.end());
		data.queries.erase(std::unique(data.queries.begin(), data.queries.end()), data.queries.end());
		for(const PhysicsQueryHandle& query : data.queries) {
			PhysicsPreparationWorkItem item;
			item.kind = PhysicsPreparationWorkKind::Query;
			item.stableOrdinal = data.workItems.size();
			item.query = query;
			data.workItems.push_back(item);
		}

		data.estimatedWorkItems = data.workItems.size();
		if(data.workItems.empty()) {
			data.executionPath = PhysicsPreparationExecutionPath::Fast;
		} else if(options.availableWorkerCount <= 1U
			|| data.workItems.size() < std::max<std::size_t>(1U, options.parallelThreshold)) {
			data.executionPath = PhysicsPreparationExecutionPath::Inline;
		} else {
			data.executionPath = PhysicsPreparationExecutionPath::WorkerRanges;
		}
		const std::size_t balancedRangeSize = data.executionPath == PhysicsPreparationExecutionPath::WorkerRanges
			? (data.workItems.size() + options.availableWorkerCount - 1U) / options.availableWorkerCount
			: data.workItems.size();
		BuildRanges(data, data.executionPath == PhysicsPreparationExecutionPath::WorkerRanges
			? std::min(std::max<std::size_t>(1U, options.rangeSize), balancedRangeSize)
			: std::max<std::size_t>(1U, balancedRangeSize));
		return Create(std::move(data));
	}

	std::optional<PhysicsPreparationPlan> PhysicsPreparationPlan::Create(
		PhysicsPreparationPlanData value) {
		if(!value.world.IsValid() || value.authoringGeneration == 0U
			|| value.executionPath == PhysicsPreparationExecutionPath::Rejected
			|| !value.rejectionReason.empty()) {
			return std::nullopt;
		}
		if(!ValidateOperations(value.creates, false)
			|| !ValidateOperations(value.removals, true)
			|| !ValidateOperations(value.updates, false)
			|| !ValidateOperations(value.dependencyRebuilds, false)
			|| !ValidateOperations(value.constraints, false, true)
			|| !ValidateOperations(value.characters, false, true)) {
			return std::nullopt;
		}

		std::unordered_set<TerraEngine::UUID> bodyTargets;
		for(const auto* operations : {&value.creates, &value.removals, &value.updates, &value.dependencyRebuilds}) {
			for(const PhysicsPreparationOperation& operation : *operations) {
				if(!bodyTargets.insert(operation.objectId).second) return std::nullopt;
			}
		}
		if(value.estimatedWorkItems != value.workItems.size()
			|| (value.executionPath == PhysicsPreparationExecutionPath::Fast) != value.workItems.empty()) {
			return std::nullopt;
		}
		for(std::size_t index = 0U; index < value.workItems.size(); ++index) {
			if(value.workItems[index].stableOrdinal != index) return std::nullopt;
			if(value.workItems[index].kind == PhysicsPreparationWorkKind::Command
				&& value.workItems[index].commandId == 0U) return std::nullopt;
			if(value.workItems[index].kind == PhysicsPreparationWorkKind::Query
				&& !value.workItems[index].query.IsValid()) return std::nullopt;
		}
		std::size_t expectedBegin = 0U;
		for(std::size_t rangeIndex = 0U; rangeIndex < value.ranges.size(); ++rangeIndex) {
			const PhysicsPreparationRange& range = value.ranges[rangeIndex];
			if(range.count == 0U || range.begin != expectedBegin
				|| range.stableRangeOrdinal != rangeIndex
				|| range.begin + range.count > value.workItems.size()) {
				return std::nullopt;
			}
			for(std::size_t itemIndex = range.begin; itemIndex < range.begin + range.count; ++itemIndex) {
				if(value.workItems[itemIndex].kind != range.kind) return std::nullopt;
			}
			expectedBegin += range.count;
		}
		if(expectedBegin != value.workItems.size()) return std::nullopt;

		return PhysicsPreparationPlan(
			std::make_shared<const PhysicsPreparationPlanData>(std::move(value)));
	}

	PhysicsPreparationScratch::PhysicsPreparationScratch(std::size_t itemLimit)
		: maximumItems(std::max<std::size_t>(1U, itemLimit)) {}

	bool PhysicsPreparationScratch::BeginGeneration(
		PhysicsGeneration generation,
		std::size_t itemCount) {
		if(generation == 0U) return false;
		for(std::size_t index = 0U; index < activeItems && index < slots.size(); ++index) {
			slots[index] = {};
		}
		futures.clear();
		if(itemCount > maximumItems) {
			slots.resize(0U);
			activeItems = 0U;
			return false;
		}
		const std::size_t previousCapacity = slots.capacity();
		if(itemCount > previousCapacity) {
			slots.reserve(std::min(maximumItems, std::max(itemCount, previousCapacity * 2U)));
			++allocationCount;
		} else if(itemCount > 0U) {
			++reuseCount;
		}
		slots.resize(itemCount);
		activeItems = itemCount;
		highWatermark = std::max(highWatermark, itemCount);
		for(PhysicsPreparationScratchSlot& slot : slots) slot.generation = generation;
		return true;
	}

	bool ExecutePhysicsPreparationPlan(
		const PhysicsPreparationPlan& plan,
		ThreadPool& threadPool,
		PhysicsPreparationScratch& scratch,
		const std::function<bool(const PhysicsPreparationWorkItem&)>& prepare) {
		if(!plan.IsValid() || !prepare
			|| !scratch.BeginGeneration(plan.Data().authoringGeneration, plan.Data().workItems.size())) {
			return false;
		}
		if(plan.Empty()) return true;

		std::atomic_bool accepted = true;
		auto executeRange = [&](const PhysicsPreparationRange& range) {
			for(std::size_t index = range.begin; index < range.begin + range.count; ++index) {
				PhysicsPreparationScratchSlot& slot = scratch.slots[index];
				slot.item = plan.Data().workItems[index];
				slot.attempted = true;
				try {
					slot.accepted = prepare(slot.item);
				} catch(...) {
					slot.accepted = false;
				}
				if(!slot.accepted) accepted.store(false, std::memory_order_release);
			}
		};

		if(plan.Data().executionPath != PhysicsPreparationExecutionPath::WorkerRanges
			|| threadPool.getThreadCount() <= 1U) {
			for(const PhysicsPreparationRange& range : plan.Data().ranges) executeRange(range);
			return accepted.load(std::memory_order_acquire);
		}

		try {
			scratch.futures.reserve(plan.Data().ranges.size());
		} catch(...) {
			return false;
		}
		for(const PhysicsPreparationRange& range : plan.Data().ranges) {
			try {
				auto task = threadPool.createTask([range, &executeRange]() { executeRange(range); });
				std::future<void> future = task->getFuture();
				threadPool.enqueue(std::move(task));
				scratch.futures.push_back(std::move(future));
			} catch(...) {
				// Previously queued ranges still reference executeRange, so retain this
				// stack frame and wait for them before reporting the rejected preparation.
				accepted.store(false, std::memory_order_release);
				break;
			}
		}
		for(std::future<void>& future : scratch.futures) {
			try {
				future.get();
			} catch(...) {
				accepted.store(false, std::memory_order_release);
			}
		}
		return accepted.load(std::memory_order_acquire);
	}
}
