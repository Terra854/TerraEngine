#include "VirtualGeometryAssemblyBuilder.h"
#include <algorithm>
namespace TerraEngine::Editor
{
	static std::uint64_t StableAssemblyId(std::uint64_t source, std::size_t ordinal) noexcept
	{
		// FNV-style mixing keeps IDs reproducible while separating equal source IDs
		// that belong to different disconnected assembly clusters.
		std::uint64_t hash = 1469598103934665603ull ^ source;
		hash ^= static_cast<std::uint64_t>(ordinal) + 0x9e3779b97f4a7c15ull;
		return hash == 0 ? 1 : hash;
	}

	std::vector<TerraEngine::Renderer::VirtualGeometryAggregateRepresentation> BuildVirtualGeometryAssemblyCandidates(std::vector<std::uint64_t> ids)
	{
		std::sort(ids.begin(), ids.end());
		std::vector<TerraEngine::Renderer::VirtualGeometryAggregateRepresentation> r;
		ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
		r.reserve(ids.size());
		for (std::size_t ordinal = 0; ordinal < ids.size(); ++ordinal)
		{
			const auto id = StableAssemblyId(ids[ordinal], ordinal);
			r.push_back({ TerraEngine::Renderer::VirtualGeometryAggregateKind::Assembly, id,
				0.01f * static_cast<float>(ordinal + 1), true, 1u, 1.0f, id,
				ids[ordinal], id, true, true });
		}
		return r;
	}
} // namespace TerraEngine::Editor
