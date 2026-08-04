#include "GpuDeformationPlan.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace TerraEngine::Renderer {

	namespace {
		bool HasReusableContent(const DeformationPageDemand& demand) noexcept
		{
			return demand.key.deformation.contentIdLow != 0u
				&& demand.key.deformation.contentIdHigh != 0u;
		}

		bool SameReusableKey(
			const DeformationPageDemand& left,
			const DeformationPageDemand& right) noexcept
		{
			if(left.key.page != right.key.page
				|| left.key.inputLayoutVersion != right.key.inputLayoutVersion
				|| left.key.animationLod != right.key.animationLod
				|| left.key.animationLodVersion != right.key.animationLodVersion)
			{
				return false;
			}
			if(!HasReusableContent(left) || !HasReusableContent(right))
			{
				return left.key == right.key;
			}
			return left.key.deformation.contentIdLow
					== right.key.deformation.contentIdLow
				&& left.key.deformation.contentIdHigh
					== right.key.deformation.contentIdHigh
				&& left.key.deformation.rendererGeneration
					== right.key.deformation.rendererGeneration
				&& left.key.deformation.sceneGeneration
					== right.key.deformation.sceneGeneration;
		}

		bool ReusableKeyLess(
			const DeformationPageDemand& left,
			const DeformationPageDemand& right) noexcept
		{
			if(left.key.page != right.key.page) return left.key.page < right.key.page;
			if(left.key.inputLayoutVersion != right.key.inputLayoutVersion)
				return left.key.inputLayoutVersion < right.key.inputLayoutVersion;
			if(left.key.animationLod != right.key.animationLod)
				return left.key.animationLod < right.key.animationLod;
			if(left.key.animationLodVersion != right.key.animationLodVersion)
				return left.key.animationLodVersion < right.key.animationLodVersion;
			const bool leftReusable = HasReusableContent(left);
			const bool rightReusable = HasReusableContent(right);
			if(leftReusable != rightReusable) return leftReusable < rightReusable;
			if(!leftReusable) return left.key < right.key;
			if(left.key.deformation.contentIdLow
				!= right.key.deformation.contentIdLow)
			{
				return left.key.deformation.contentIdLow
					< right.key.deformation.contentIdLow;
			}
			if(left.key.deformation.contentIdHigh
				!= right.key.deformation.contentIdHigh)
			{
				return left.key.deformation.contentIdHigh
					< right.key.deformation.contentIdHigh;
			}
			if(left.key.deformation.rendererGeneration
				!= right.key.deformation.rendererGeneration)
			{
				return left.key.deformation.rendererGeneration
					< right.key.deformation.rendererGeneration;
			}
			if(left.key.deformation.sceneGeneration
				!= right.key.deformation.sceneGeneration)
			{
				return left.key.deformation.sceneGeneration
					< right.key.deformation.sceneGeneration;
			}
			// Equal reusable keys use owner identity only as a deterministic tie-break;
			// SameReusableKey still merges them into one dispatch below.
			const std::uint64_t leftOwner = left.ownerStableInstanceId != 0u
				? left.ownerStableInstanceId : left.key.deformation.stableInstanceId;
			const std::uint64_t rightOwner = right.ownerStableInstanceId != 0u
				? right.ownerStableInstanceId : right.key.deformation.stableInstanceId;
			return leftOwner < rightOwner;
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

		bool SameWorkShape(const DeformationPageDemand& left, const DeformationPageDemand& right)
		{
			return (left.sourceRecordIndex == right.sourceRecordIndex
					|| (HasReusableContent(left) && HasReusableContent(right)))
				&& left.vertexCount == right.vertexCount
				&& left.meshletCount == right.meshletCount
				&& left.clusterCount == right.clusterCount;
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
		return std::all_of(work.begin(), work.end(),
			[this](const DeformationWorkRecord& record)
			{
				return record.key.IsComplete()
					&& record.key.deformation.rendererGeneration == rendererGeneration
					&& record.sourceRecordIndex != kInvalidDeformationRecordIndex
					&& record.sourceStableInstanceId != 0u
					&& !record.consumerStableInstanceIds.empty()
					&& record.vertexCount != 0u
					&& record.meshletCount != 0u
					&& record.clusterCount != 0u
					&& record.requestedViewMask != 0u;
			});
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

		std::vector<DeformationPageDemand> ordered(demands.begin(), demands.end());
		std::sort(ordered.begin(), ordered.end(), ReusableKeyLess);

		for(std::size_t index = 0u; index < ordered.size();)
		{
			const DeformationPageDemand& first = ordered[index];
			if(!first.key.IsComplete()
				|| first.key.deformation.rendererGeneration != rendererGeneration
				|| first.sourceRecordIndex == kInvalidDeformationRecordIndex
				|| (first.ownerStableInstanceId == 0u
					&& first.key.deformation.stableInstanceId == 0u)
				|| (first.sourceStableInstanceId == 0u
					&& first.key.deformation.stableInstanceId == 0u)
				|| first.vertexCount == 0u
				|| first.meshletCount == 0u
				|| first.clusterCount == 0u
				|| first.requestedViewMask == 0u)
			{
				reject("A page demand carried an incomplete identity, range, source, or view mask.");
				return plan;
			}

			DeformationPageDemand merged = first;
			std::size_t next = index + 1u;
			for(; next < ordered.size()
				&& SameReusableKey(ordered[next], first); ++next)
			{
				if(!SameWorkShape(first, ordered[next]))
				{
					reject("Compatible page identities described conflicting source or output ranges.");
					return plan;
				}
				merged.requestedViewMask |= ordered[next].requestedViewMask;
				merged.priorHistoryCompatible = merged.priorHistoryCompatible
					&& ordered[next].priorHistoryCompatible;
				++plan.reusedDemandCount;
			}

			DeformationWorkRecord record;
			record.key = merged.key;
			record.sourceStableInstanceId = merged.sourceStableInstanceId != 0u
				? merged.sourceStableInstanceId
				: merged.key.deformation.stableInstanceId;
			for(std::size_t consumerIndex = index;
				consumerIndex < next; ++consumerIndex)
			{
				const std::uint64_t owner =
					ordered[consumerIndex].ownerStableInstanceId != 0u
						? ordered[consumerIndex].ownerStableInstanceId
						: ordered[consumerIndex].key.deformation.stableInstanceId;
				record.consumerStableInstanceIds.push_back(owner);
			}
			std::sort(record.consumerStableInstanceIds.begin(),
				record.consumerStableInstanceIds.end());
			record.consumerStableInstanceIds.erase(std::unique(
				record.consumerStableInstanceIds.begin(),
				record.consumerStableInstanceIds.end()),
				record.consumerStableInstanceIds.end());
			record.sourceRecordIndex = merged.sourceRecordIndex;
			record.vertexOffset = plan.totalVertexCount;
			record.vertexCount = merged.vertexCount;
			record.meshletBoundsOffset = plan.totalMeshletBoundsCount;
			record.meshletCount = merged.meshletCount;
			record.clusterBoundsOffset = plan.totalClusterBoundsCount;
			record.clusterCount = merged.clusterCount;
			record.requestedViewMask = merged.requestedViewMask;
			record.historyValid = merged.priorHistoryCompatible;

			if(!TryAddCount(plan.totalVertexCount, record.vertexCount, plan.totalVertexCount)
				|| !TryAddCount(plan.totalMeshletBoundsCount, record.meshletCount,
					plan.totalMeshletBoundsCount)
				|| !TryAddCount(plan.totalClusterBoundsCount, record.clusterCount,
					plan.totalClusterBoundsCount))
			{
				reject("Deformation output prefix sums exceeded the v1 32-bit shader range.");
				return plan;
			}

			plan.work.push_back(std::move(record));
			index = next;
		}

		return plan;
	}

}
