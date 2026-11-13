#include "Physics/Terrain/TerrainCollisionSeamPreparation.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/Backend/PhysicsAllocatorAdapter.h"
#include "ThreadPool/ThreadPool.h"
#include <array>
#include <limits>

namespace TerraEngine::Physics::Terrain
{
	bool PrepareTerrainCollisionSeams(TerrainHeightFieldRecipe& recipe,
		const TerrainCollisionRootCapture& capture)
	{
		if(!recipe.IsComplete() || recipe.terrainAssetId != capture.assetId
			|| !capture.rootHash.IsValid() || capture.generation == 0u
			|| !capture.areas.contains({recipe.areaX, recipe.areaZ})) return false;
		// Work in a detached copy so malformed captured data cannot partially mask
		// the caller's previously valid candidate.
		auto prepared = recipe;
		std::array<bool, TerrainHeightFieldRecipe::kSampleCount> supported{};
		prepared.collisionSourceOwners.clear();
		prepared.cookedCollision.reset();
		for(std::uint32_t dz = 0u; dz < 2u; ++dz)
			for(std::uint32_t dx = 0u; dx < 2u; ++dx)
			{
				if((dx && recipe.areaX == std::numeric_limits<std::int64_t>::max())
					|| (dz && recipe.areaZ == std::numeric_limits<std::int64_t>::max())) continue;
				const auto found = capture.areas.find({recipe.areaX + dx, recipe.areaZ + dz});
				if(found == capture.areas.end()) continue;
				const auto& area = found->second;
				if(!area.active.IsValid() || area.active.maxXExclusive > 256u
					|| area.active.maxZExclusive > 256u || !area.heights
					|| area.heights->size() != 256u * 256u) return false;
				prepared.collisionSourceOwners.push_back(area.heights);
				// Positive neighbors contribute only their first edge or corner. The
				// lower-address area owns each seam cell, avoiding double publication.
				for(std::uint32_t z = 0u; z < (dz ? 1u : 256u); ++z)
					for(std::uint32_t x = 0u; x < (dx ? 1u : 256u); ++x)
					{
						if(x < area.active.minX || z < area.active.minZ
							|| x >= area.active.maxXExclusive || z >= area.active.maxZExclusive) continue;
						const auto index = static_cast<std::size_t>(z + dz * 256u) * 257u + x + dx * 256u;
						prepared.heightMillimetres[index] = (*area.heights)[z * 256u + x];
						supported[index] = true;
					}
			}
		for(std::uint32_t z = 0u; z < 256u; ++z)
			for(std::uint32_t x = 0u; x < 256u; ++x)
			{
				const auto index = static_cast<std::size_t>(z) * 257u + x;
				if(supported[index] && supported[index + 1u]
					&& supported[index + 257u] && supported[index + 258u]) continue;
				const auto cell = z * 256u + x;
				prepared.holeBits[cell / 8u] |= static_cast<std::uint8_t>(1u << (cell % 8u));
			}
		// A neighbor-only edit changes the root hash/generation, invalidating the
		// provider cache without pretending that this area's authored bytes changed.
		prepared.collisionDependencyHash = capture.rootHash.low64 ^ capture.rootHash.high64
			^ capture.generation ^ static_cast<std::uint64_t>(recipe.areaX)
			^ (static_cast<std::uint64_t>(recipe.areaZ) * 0x9e3779b97f4a7c15ULL);
		if(prepared.collisionDependencyHash == 0u) prepared.collisionDependencyHash = 1u;
		prepared.collisionSourceRootLow = capture.rootHash.low64;
		prepared.collisionSourceRootHigh = capture.rootHash.high64;
		prepared.collisionSourceGeneration = capture.generation;
		recipe = std::move(prepared);
		return true;
	}

}

namespace TerraEngine::Physics
{
	std::uint64_t TerrainCollisionRecipeHash(const TerrainHeightFieldRecipe& recipe)
	{
		// Hash all derived geometry/settings as well as source identity. Missing
		// neighbor masks, holes and neighbor-only height edits therefore cannot reuse
		// an unrelated cached provider shape. Version includes the mesh diagonal,
		// exact eligible-sample mask, shared NW/SE diagonal, 16-bit heightfield
		// precision and two-sample Jolt block cooking policy.
		std::uint64_t hash = 14695981039346656037ULL;
		const auto add = [&hash](const void* data, std::size_t size) {
			const auto* bytes = static_cast<const unsigned char*>(data);
			for(std::size_t i = 0u; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
		};
		const std::uint64_t version = 0x5445525241494e03ULL;
		add(&version, sizeof(version));
		add(&recipe.collisionDependencyHash, sizeof(recipe.collisionDependencyHash));
		add(&recipe.collisionSourceRootLow, sizeof(recipe.collisionSourceRootLow));
		add(&recipe.collisionSourceRootHigh, sizeof(recipe.collisionSourceRootHigh));
		add(&recipe.collisionSourceGeneration, sizeof(recipe.collisionSourceGeneration));
		add(&recipe.areaX, sizeof(recipe.areaX));
		add(&recipe.areaZ, sizeof(recipe.areaZ));
		add(&recipe.authoredHash, sizeof(recipe.authoredHash));
		add(&recipe.authoredGeneration, sizeof(recipe.authoredGeneration));
		add(&recipe.sampleSpacingMetres, sizeof(recipe.sampleSpacingMetres));
		add(&recipe.heightDatumMetres, sizeof(recipe.heightDatumMetres));
		add(recipe.heightMillimetres.data(), recipe.heightMillimetres.size() * sizeof(std::int32_t));
		add(recipe.holeBits.data(), recipe.holeBits.size());
		add(recipe.materialIndices.data(), recipe.materialIndices.size() * sizeof(std::uint16_t));
		return hash == 0u ? 1u : hash;
	}
}

namespace TerraEngine::Physics::Terrain
{
	bool CookTerrainCollisionRecipe(TerrainHeightFieldRecipe& recipe)
	{
		if(!recipe.IsComplete()) return false;
		const auto hash = TerrainCollisionRecipeHash(recipe);
		if(recipe.cookedCollision) return recipe.cookedCollision->payload
			&& !recipe.cookedCollision->payload->empty() && recipe.cookedCollision->recipeHash == hash;
		// Shape cooking is synchronous inside this worker. Attribute its provider
		// failures locally, including backend teardown, while live-world jobs on
		// other threads keep their own fatal-health reporting unchanged.
		Backend::ScopedPhysicsProviderFailureIsolation failureIsolation;
		Backend::JoltPhysicsBackendConfiguration config{};
		config.maximumPhysicsWorkers = 1u;
		config.temporaryAllocatorBytes = 1024u * 1024u;
		Backend::JoltPhysicsBackend cooker(ThreadPool::instance(), config);
		if(!cooker.Initialize().Succeeded()) return false;
		auto source = Backend::MakeTerrainHeightFieldShapeRecipe(recipe);
		source.stableRecipeHash = hash;
		auto result = cooker.CookShape({{}, std::move(source), "terrain-worker-collision"});
		if(!result.result.Succeeded()) return false;
		auto cooked = std::make_shared<TerrainCollisionCookedData>();
		cooked->payload = std::make_shared<const std::vector<std::byte>>(std::move(result.payload));
		cooked->formatFamily = result.formatFamily; cooked->provider = result.provider;
		cooked->providerVersion = result.providerVersion; cooked->providerRevision = result.providerRevision;
		cooked->formatVersion = result.formatVersion; cooked->checksum = result.payloadChecksum;
		cooked->recipeHash = hash;
		const bool mesh = std::any_of(recipe.holeBits.begin(), recipe.holeBits.end(), [](auto bits) { return bits != 0u; });
		const auto supported = mesh ? 65536u - [&recipe] {
			std::size_t disabled = 0u;
			for(const auto bits : recipe.holeBits)
				for(unsigned bit = 0u; bit < 8u; ++bit) disabled += (bits >> bit) & 1u;
			return disabled;
		}() : 0u;
		// Conservative worker geometry-buffer bound: holes may need the full mesh,
		// while representable boundary masks allocate only missing samples + HF
		// floats. Vendor memory is separately observed after actual shape adoption.
		cooked->derivedGeometryBytes = mesh ? 66049u + 66049u * 3u * sizeof(float) + supported * 2u * 5u * sizeof(std::uint32_t)
			: 66049u * sizeof(float);
		recipe.cookedCollision = std::move(cooked);
		return true;
	}
}
