#include "VirtualGeometryAssemblyBuilder.h"
#include <algorithm>
#include <unordered_set>
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
		// Reserve every original identity first. A collision fallback must not
		// steal a later piece's ordinary ID and unnecessarily change its references.
		std::unordered_set<std::uint64_t> reserved, allocated;
		for(std::size_t ordinal = 0; ordinal < ids.size(); ++ordinal) reserved.insert(StableAssemblyId(ids[ordinal], ordinal));
		for (std::size_t ordinal = 0; ordinal < ids.size(); ++ordinal)
		{
			auto id = StableAssemblyId(ids[ordinal], ordinal);
			if(!allocated.insert(id).second) {
				// Mix the complete source/ordinal tuple byte by byte only on collision.
				// Sorted traversal plus deterministic probing makes input permutations
				// reproducible, including repeated source pieces with distinct ordinals.
				id = 14695981039346656037ull;
				for(const std::uint64_t word : {ids[ordinal], static_cast<std::uint64_t>(ordinal)})
					for(unsigned shift = 0; shift < 64U; shift += 8U) { id ^= (word >> shift) & 0xffU; id *= 1099511628211ull; }
				while(id == 0U || reserved.contains(id) || allocated.contains(id)) ++id;
				allocated.insert(id);
			}
			r.push_back({ TerraEngine::Renderer::VirtualGeometryAggregateKind::Assembly, id,
				0.01f * static_cast<float>(ordinal + 1), true, 1u, 1.0f, id,
				ids[ordinal], id, true, true });
		}
		return r;
	}
} // namespace TerraEngine::Editor
