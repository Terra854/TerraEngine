#include "RenderFrameSnapshot.h"

#include <functional>

namespace TerraEngine::Renderer {

	namespace {
		void HashCombine(std::uint64_t& seed, std::uint64_t value)
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		std::uint64_t HashUuid(const TerraEngine::UUID& id)
		{
			return static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(id));
		}

		template<typename Record>
		void SortByUuid(std::vector<Record>& records, TerraEngine::UUID Record::* member)
		{
			std::sort(records.begin(), records.end(), [member](const Record& left, const Record& right) {
				return TerraEngine::ToHexString(left.*member) < TerraEngine::ToHexString(right.*member);
			});
		}
	}

	void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot)
	{
		// Stable sort points make repeated frames produce identical work packet
		// ids and pass ordering even when scene registry iteration order changes.
		std::sort(snapshot.viewRequests.begin(), snapshot.viewRequests.end(), [](const RenderFrameViewRequest& left, const RenderFrameViewRequest& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		SortByUuid(snapshot.renderables, &RenderFrameRenderableRecord::objectId);
		SortByUuid(snapshot.lights, &RenderFrameLightRecord::lightId);
		SortByUuid(snapshot.materials, &RenderFrameResourceRecord::resourceId);
		SortByUuid(snapshot.meshes, &RenderFrameResourceRecord::resourceId);
		SortByUuid(snapshot.textures, &RenderFrameResourceRecord::resourceId);
	}

	bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot)
	{
		auto allResourcesResolved = [](const std::vector<RenderFrameResourceRecord>& records) {
			return std::all_of(records.begin(), records.end(), [](const RenderFrameResourceRecord& record) {
				return record.resourceId != TerraEngine::EMPTY_UUID && (record.resident || record.usingFallback);
			});
		};

		return snapshot.valid
			&& !SnapshotContainsLiveMutableState(snapshot)
			&& allResourcesResolved(snapshot.materials)
			&& allResourcesResolved(snapshot.meshes)
			&& allResourcesResolved(snapshot.textures);
	}

	bool SnapshotContainsLiveMutableState(const RenderFrameSnapshot& snapshot)
	{
		return snapshot.containsLiveMutableState;
	}

	std::uint64_t BuildSnapshotDeterministicHash(const RenderFrameSnapshot& snapshot)
	{
		std::uint64_t hash = snapshot.frameId;
		HashCombine(hash, snapshot.sceneRevision);
		HashCombine(hash, snapshot.resourceRevision);
		HashCombine(hash, snapshot.valid ? 1u : 0u);
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			HashCombine(hash, view.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(view.viewType));
			HashCombine(hash, view.targetSize.x);
			HashCombine(hash, view.targetSize.y);
		}
		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			HashCombine(hash, HashUuid(renderable.objectId));
			HashCombine(hash, HashUuid(renderable.meshId));
			HashCombine(hash, HashUuid(renderable.materialId));
			HashCombine(hash, renderable.taskShaderCullingEnabled ? 1u : 0u);
			HashCombine(hash, renderable.usingFallback ? 1u : 0u);
		}
		for (const RenderFrameLightRecord& light : snapshot.lights)
		{
			HashCombine(hash, HashUuid(light.lightId));
			HashCombine(hash, light.lightType);
			HashCombine(hash, light.castsShadows ? 1u : 0u);
		}
		return hash;
	}

	std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot)
	{
		// The estimate is intentionally backend-neutral. It reflects CPU work
		// units the graph can split, not GPU timings or API-specific commands.
		return static_cast<std::uint64_t>(snapshot.renderables.size())
			+ static_cast<std::uint64_t>(snapshot.lights.size())
			+ static_cast<std::uint64_t>(snapshot.viewRequests.size() * 8u);
	}

}
