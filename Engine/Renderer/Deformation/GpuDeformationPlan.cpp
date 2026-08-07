#include "GpuDeformationPlan.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Renderer {

	namespace {
		bool HasReusableContent(const DeformedPageKey& key) noexcept
		{
			return key.deformation.contentIdLow != 0u
				&& key.deformation.contentIdHigh != 0u;
		}

		bool SameReusableKey(
			const DeformedPageKey& left,
			const DeformedPageKey& right) noexcept
		{
			if(left.page != right.page
				|| left.inputLayoutVersion != right.inputLayoutVersion
				|| left.animationLod != right.animationLod
				|| left.animationLodVersion != right.animationLodVersion)
			{
				return false;
			}
			if(!HasReusableContent(left) || !HasReusableContent(right))
			{
				return left == right;
			}
			return left.deformation.contentIdLow == right.deformation.contentIdLow
				&& left.deformation.contentIdHigh == right.deformation.contentIdHigh
				&& left.deformation.rendererGeneration == right.deformation.rendererGeneration
				&& left.deformation.sceneGeneration == right.deformation.sceneGeneration;
		}

		bool TryAddCount(std::uint32_t current, std::uint32_t additional, std::uint32_t& result)
		{
			if(additional > std::numeric_limits<std::uint32_t>::max() - current)
			{
				return false;
			}
			result = current + additional;
			return true;
		}

		bool SameWorkShape(
			const DeformationWorkRecord& representative,
			const DeformationPageDemand& demand)
		{
			return (representative.sourceRecordIndex == demand.sourceRecordIndex
					|| (HasReusableContent(representative.key)
						&& HasReusableContent(demand.key)))
				&& representative.vertexCount == demand.vertexCount
				&& representative.meshletCount == demand.meshletCount
				&& representative.clusterCount == demand.clusterCount;
		}

		void HashReusableValue(std::size_t& hash, std::uint64_t value) noexcept
		{
			hash ^= static_cast<std::size_t>(value ^ (value >> 32u))
				+ 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
		}

		std::size_t ReusableKeyFingerprint(const DeformationPageDemand& demand) noexcept
		{
			// A fingerprint only chooses a small candidate bucket; SameReusableKey
			// performs the complete identity check before any work is shared. Hash the
			// high-entropy mesh, page, and content/owner fields here so crowd clones do
			// not repeatedly mix frame-constant generations and ABI values.
			std::size_t hash = std::hash<TerraEngine::UUID>{}(demand.key.page.meshId);
			HashReusableValue(hash, demand.key.page.pageId);
			const DeformationGeneration& generation = demand.key.deformation;
			if(HasReusableContent(demand.key))
			{
				HashReusableValue(hash, generation.contentIdLow);
				HashReusableValue(hash, generation.contentIdHigh);
			}
			else
			{
				HashReusableValue(hash, generation.stableInstanceId);
			}
			return hash;
		}

		void FinalizeConsumerOrder(GpuDeformationPlan& plan)
		{
			std::size_t consumerCount = 0u;
			for(const DeformationWorkRecord& work : plan.work)
			{
				consumerCount += work.ConsumerCount();
			}
			plan.orderedConsumers.clear();
			plan.orderedConsumers.reserve(consumerCount);
			for(std::uint32_t workIndex = 0u; workIndex < plan.work.size(); ++workIndex)
			{
				DeformationWorkRecord& work = plan.work[workIndex];
				plan.orderedConsumers.push_back({
					work.primaryConsumerStableInstanceId, workIndex });
				for(const std::uint64_t consumer : work.additionalConsumerStableInstanceIds)
				{
					plan.orderedConsumers.push_back({consumer, workIndex});
				}
				work.additionalConsumerStableInstanceIds.clear();
			}
			const auto orderedBefore = [&plan](
				const DeformationConsumerWorkRecord& left,
				const DeformationConsumerWorkRecord& right)
			{
				const DeformationWorkRecord& leftWork = plan.work[left.workIndex];
				const DeformationWorkRecord& rightWork = plan.work[right.workIndex];
				if(left.stableInstanceId != right.stableInstanceId)
				{
					return left.stableInstanceId < right.stableInstanceId;
				}
				if(leftWork.key.page.meshId != rightWork.key.page.meshId)
				{
					return leftWork.key.page.meshId < rightWork.key.page.meshId;
				}
				return left.workIndex < right.workIndex;
			};
			if(!std::is_sorted(plan.orderedConsumers.begin(),
				plan.orderedConsumers.end(), orderedBefore))
			{
				// Stable instance identity is the dominant ordering lane. Four 16-bit
				// passes keep crowd ordering linear; the small equal-owner runs then sort
				// by mesh and work index to preserve the complete public contract.
				thread_local std::vector<DeformationConsumerWorkRecord> radixScratch;
				thread_local std::vector<std::size_t> radixOffsets(1u << 16u, 0u);
				radixScratch.resize(plan.orderedConsumers.size());
				for(std::uint32_t pass = 0u; pass < 4u; ++pass)
				{
					std::fill(radixOffsets.begin(), radixOffsets.end(), 0u);
					const std::uint32_t shift = pass * 16u;
					const auto& source = (pass & 1u) == 0u
						? plan.orderedConsumers : radixScratch;
					auto& destination = (pass & 1u) == 0u
						? radixScratch : plan.orderedConsumers;
					for(const DeformationConsumerWorkRecord& consumer : source)
					{
						++radixOffsets[static_cast<std::uint16_t>(
							consumer.stableInstanceId >> shift)];
					}
					std::size_t offset = 0u;
					for(std::size_t& count : radixOffsets)
					{
						const std::size_t next = offset + count;
						count = offset;
						offset = next;
					}
					for(const DeformationConsumerWorkRecord& consumer : source)
					{
						destination[radixOffsets[static_cast<std::uint16_t>(
							consumer.stableInstanceId >> shift)]++] = consumer;
					}
				}
				for(auto begin = plan.orderedConsumers.begin();
					begin != plan.orderedConsumers.end();)
				{
					auto end = std::find_if(begin, plan.orderedConsumers.end(),
						[stableInstanceId = begin->stableInstanceId](
							const DeformationConsumerWorkRecord& consumer)
						{
							return consumer.stableInstanceId != stableInstanceId;
						});
					if(end - begin > 1)
					{
						std::sort(begin, end, orderedBefore);
					}
					begin = end;
				}
			}
			plan.orderedConsumers.erase(std::unique(
				plan.orderedConsumers.begin(), plan.orderedConsumers.end(),
				[](const DeformationConsumerWorkRecord& left,
					const DeformationConsumerWorkRecord& right)
				{
					return left.stableInstanceId == right.stableInstanceId
						&& left.workIndex == right.workIndex;
				}), plan.orderedConsumers.end());
			for(const DeformationConsumerWorkRecord& consumer : plan.orderedConsumers)
			{
				DeformationWorkRecord& work = plan.work[consumer.workIndex];
				if(consumer.stableInstanceId != work.primaryConsumerStableInstanceId)
				{
					work.additionalConsumerStableInstanceIds.push_back(
						consumer.stableInstanceId);
				}
			}
		}

	}

	bool DeformationGeneration::IsComplete() const noexcept
	{
		return stableInstanceId != 0u
			&& objectGeneration != 0u
			&& animationPublicationGeneration != 0u
			&& skeletonGeneration != 0u
			&& morphGeneration != 0u
			&& meshPayloadGeneration != 0u
			&& rendererGeneration != 0u
			&& sceneGeneration != 0u
			&& ((contentIdLow == 0u) == (contentIdHigh == 0u));
	}

	bool StaticPageKey::IsComplete() const noexcept
	{
		return meshId != TerraEngine::EMPTY_UUID
			&& meshPayloadGeneration != 0u
			&& pageId != kInvalidDeformationRecordIndex
			&& pageContentGeneration != 0u
			&& residencyGeneration != 0u
			&& layoutVersion == kAnimatedGeometryAbiVersion
			&& codecProfile != 0u
			&& codecVersion != 0u;
	}

	bool DeformedPageKey::IsComplete() const noexcept
	{
		return page.IsComplete()
			&& deformation.IsComplete()
			&& deformation.meshPayloadGeneration == page.meshPayloadGeneration
			&& inputLayoutVersion == kAnimatedGeometryAbiVersion
			&& animationLodVersion == kAnimatedGeometryAbiVersion;
	}

	bool GpuDeformationPlan::IsComplete() const noexcept
	{
		if(!valid || frameId == 0u || rendererGeneration == 0u)
		{
			return false;
		}
		if(sealedComplete)
		{
			return true;
		}
		return std::all_of(work.begin(), work.end(),
			[this](const DeformationWorkRecord& record)
			{
				return record.key.IsComplete()
					&& record.key.deformation.rendererGeneration == rendererGeneration
					&& record.sourceRecordIndex != kInvalidDeformationRecordIndex
					&& record.sourceStableInstanceId != 0u
					&& record.primaryConsumerStableInstanceId != 0u
					&& record.vertexCount != 0u
					&& record.meshletCount != 0u
					&& record.clusterCount != 0u
					&& record.requestedViewMask != 0u;
			});
	}

	struct GpuDeformationPlanAccumulator::Impl
	{
		GpuDeformationPlan plan;
		std::unordered_multimap<std::size_t, std::size_t> reusableWorkLookup;

		void Reject(std::string reason)
		{
			plan.valid = false;
			plan.sealedComplete = false;
			plan.rejectionReason = std::move(reason);
			plan.work.clear();
			plan.totalVertexCount = 0u;
			plan.totalMeshletBoundsCount = 0u;
			plan.totalClusterBoundsCount = 0u;
			plan.reusedDemandCount = 0u;
		}
	};

	GpuDeformationPlanAccumulator::GpuDeformationPlanAccumulator(
		std::uint64_t frameId,
		std::uint64_t rendererGeneration,
		std::size_t expectedDemandCount)
		: impl(std::make_unique<Impl>())
	{
		impl->plan.frameId = frameId;
		impl->plan.rendererGeneration = rendererGeneration;
		impl->plan.work.reserve(expectedDemandCount);
		impl->reusableWorkLookup.reserve(expectedDemandCount);
		if(frameId == 0u || rendererGeneration == 0u)
		{
			impl->Reject("Deformation planning requires nonzero frame and renderer generations.");
		}
	}

	GpuDeformationPlanAccumulator::~GpuDeformationPlanAccumulator() = default;
	GpuDeformationPlanAccumulator::GpuDeformationPlanAccumulator(
		GpuDeformationPlanAccumulator&&) noexcept = default;
	GpuDeformationPlanAccumulator& GpuDeformationPlanAccumulator::operator=(
		GpuDeformationPlanAccumulator&&) noexcept = default;

	bool GpuDeformationPlanAccumulator::Add(const DeformationPageDemand& demand)
	{
		if(!impl || !impl->plan.valid)
		{
			return false;
		}
		GpuDeformationPlan& plan = impl->plan;
		if(!demand.key.IsComplete()
			|| demand.key.deformation.rendererGeneration != plan.rendererGeneration
			|| demand.sourceRecordIndex == kInvalidDeformationRecordIndex
			|| (demand.ownerStableInstanceId == 0u
				&& demand.key.deformation.stableInstanceId == 0u)
			|| (demand.sourceStableInstanceId == 0u
				&& demand.key.deformation.stableInstanceId == 0u)
			|| demand.vertexCount == 0u
			|| demand.meshletCount == 0u
			|| demand.clusterCount == 0u
			|| demand.requestedViewMask == 0u)
		{
			impl->Reject("A page demand carried an incomplete identity, range, source, or view mask.");
			return false;
		}

		const std::size_t fingerprint = ReusableKeyFingerprint(demand);
		std::size_t matchingWorkIndex = std::numeric_limits<std::size_t>::max();
		const auto candidates = impl->reusableWorkLookup.equal_range(fingerprint);
		for(auto candidate = candidates.first; candidate != candidates.second; ++candidate)
		{
			const std::size_t workIndex = candidate->second;
			if(workIndex < plan.work.size()
				&& SameReusableKey(plan.work[workIndex].key, demand.key))
			{
				matchingWorkIndex = workIndex;
				break;
			}
		}
		const std::uint64_t owner = demand.ownerStableInstanceId != 0u
			? demand.ownerStableInstanceId : demand.key.deformation.stableInstanceId;
		if(matchingWorkIndex != std::numeric_limits<std::size_t>::max())
		{
			if(!SameWorkShape(plan.work[matchingWorkIndex], demand))
			{
				impl->Reject("Compatible page identities described conflicting source or output ranges.");
				return false;
			}
			DeformationWorkRecord& record = plan.work[matchingWorkIndex];
			record.requestedViewMask |= demand.requestedViewMask;
			record.historyValid = record.historyValid && demand.priorHistoryCompatible;
			record.additionalConsumerStableInstanceIds.push_back(owner);
			++plan.reusedDemandCount;
			return true;
		}

		DeformationWorkRecord record;
		record.key = demand.key;
		record.sourceStableInstanceId = demand.sourceStableInstanceId != 0u
			? demand.sourceStableInstanceId : demand.key.deformation.stableInstanceId;
		record.primaryConsumerStableInstanceId = owner;
		record.sourceRecordIndex = demand.sourceRecordIndex;
		record.vertexOffset = plan.totalVertexCount;
		record.vertexCount = demand.vertexCount;
		record.meshletBoundsOffset = plan.totalMeshletBoundsCount;
		record.meshletCount = demand.meshletCount;
		record.clusterBoundsOffset = plan.totalClusterBoundsCount;
		record.clusterCount = demand.clusterCount;
		record.requestedViewMask = demand.requestedViewMask;
		record.historyValid = demand.priorHistoryCompatible;
		if(!TryAddCount(plan.totalVertexCount, record.vertexCount, plan.totalVertexCount)
			|| !TryAddCount(plan.totalMeshletBoundsCount, record.meshletCount,
				plan.totalMeshletBoundsCount)
			|| !TryAddCount(plan.totalClusterBoundsCount, record.clusterCount,
				plan.totalClusterBoundsCount))
		{
			impl->Reject("Deformation output prefix sums exceeded the v1 32-bit shader range.");
			return false;
		}

		const std::size_t workIndex = plan.work.size();
		plan.work.push_back(std::move(record));
		impl->reusableWorkLookup.emplace(fingerprint, workIndex);
		return true;
	}

	GpuDeformationPlan GpuDeformationPlanAccumulator::Finish() &&
	{
		if(!impl)
		{
			GpuDeformationPlan rejected;
			rejected.valid = false;
			rejected.rejectionReason = "Deformation accumulator ownership was already consumed.";
			return rejected;
		}
		FinalizeConsumerOrder(impl->plan);
		impl->plan.sealedComplete = impl->plan.valid;
		return std::move(impl->plan);
	}

	GpuDeformationPlan BuildGpuDeformationPlan(
		std::uint64_t frameId,
		std::uint64_t rendererGeneration,
		std::span<const DeformationPageDemand> demands)
	{
		GpuDeformationPlan plan;
		plan.frameId = frameId;
		plan.rendererGeneration = rendererGeneration;

		const auto reject = [&plan](std::string reason)
		{
			plan.valid = false;
			plan.sealedComplete = false;
			plan.rejectionReason = std::move(reason);
			plan.work.clear();
			plan.totalVertexCount = 0u;
			plan.totalMeshletBoundsCount = 0u;
			plan.totalClusterBoundsCount = 0u;
			plan.reusedDemandCount = 0u;
		};

		if(frameId == 0u || rendererGeneration == 0u)
		{
			reject("Deformation planning requires nonzero frame and renderer generations.");
			return plan;
		}

		plan.work.reserve(demands.size());
		std::unordered_multimap<std::size_t, std::size_t> reusableWorkLookup;
		reusableWorkLookup.reserve(demands.size());

		for(const DeformationPageDemand& demand : demands)
		{
			if(!demand.key.IsComplete()
				|| demand.key.deformation.rendererGeneration != rendererGeneration
				|| demand.sourceRecordIndex == kInvalidDeformationRecordIndex
				|| (demand.ownerStableInstanceId == 0u
					&& demand.key.deformation.stableInstanceId == 0u)
				|| (demand.sourceStableInstanceId == 0u
					&& demand.key.deformation.stableInstanceId == 0u)
				|| demand.vertexCount == 0u
				|| demand.meshletCount == 0u
				|| demand.clusterCount == 0u
				|| demand.requestedViewMask == 0u)
			{
				reject("A page demand carried an incomplete identity, range, source, or view mask.");
				return plan;
			}

			const std::size_t fingerprint = ReusableKeyFingerprint(demand);
			std::size_t matchingWorkIndex = std::numeric_limits<std::size_t>::max();
			const auto candidates = reusableWorkLookup.equal_range(fingerprint);
			for(auto candidate = candidates.first; candidate != candidates.second; ++candidate)
			{
				const std::size_t workIndex = candidate->second;
				if(workIndex < plan.work.size()
					&& SameReusableKey(plan.work[workIndex].key, demand.key))
				{
					matchingWorkIndex = workIndex;
					break;
				}
			}
			const std::uint64_t owner = demand.ownerStableInstanceId != 0u
				? demand.ownerStableInstanceId : demand.key.deformation.stableInstanceId;
			if(matchingWorkIndex != std::numeric_limits<std::size_t>::max())
			{
				if(!SameWorkShape(plan.work[matchingWorkIndex], demand))
				{
					reject("Compatible page identities described conflicting source or output ranges.");
					return plan;
				}
				DeformationWorkRecord& record = plan.work[matchingWorkIndex];
				record.requestedViewMask |= demand.requestedViewMask;
				record.historyValid = record.historyValid
					&& demand.priorHistoryCompatible;
				record.additionalConsumerStableInstanceIds.push_back(owner);
				++plan.reusedDemandCount;
				continue;
			}

			DeformationWorkRecord record;
			record.key = demand.key;
			record.sourceStableInstanceId = demand.sourceStableInstanceId != 0u
				? demand.sourceStableInstanceId
				: demand.key.deformation.stableInstanceId;
			record.primaryConsumerStableInstanceId = owner;
			record.sourceRecordIndex = demand.sourceRecordIndex;
			record.vertexOffset = plan.totalVertexCount;
			record.vertexCount = demand.vertexCount;
			record.meshletBoundsOffset = plan.totalMeshletBoundsCount;
			record.meshletCount = demand.meshletCount;
			record.clusterBoundsOffset = plan.totalClusterBoundsCount;
			record.clusterCount = demand.clusterCount;
			record.requestedViewMask = demand.requestedViewMask;
			record.historyValid = demand.priorHistoryCompatible;

			if(!TryAddCount(plan.totalVertexCount, record.vertexCount, plan.totalVertexCount)
				|| !TryAddCount(plan.totalMeshletBoundsCount, record.meshletCount,
					plan.totalMeshletBoundsCount)
				|| !TryAddCount(plan.totalClusterBoundsCount, record.clusterCount,
					plan.totalClusterBoundsCount))
			{
				reject("Deformation output prefix sums exceeded the v1 32-bit shader range.");
				return plan;
			}

			const std::size_t workIndex = plan.work.size();
			plan.work.push_back(std::move(record));
			reusableWorkLookup.emplace(fingerprint, workIndex);
		}

		FinalizeConsumerOrder(plan);
		plan.sealedComplete = plan.valid;

		return plan;
	}

}
