#include "RenderFrameSnapshot.h"

namespace TerraEngine::Renderer {

	namespace {
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
			&& allResourcesResolved(snapshot.materials)
			&& allResourcesResolved(snapshot.meshes)
			&& allResourcesResolved(snapshot.textures);
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
