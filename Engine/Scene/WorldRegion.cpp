// WorldRegion.cpp - checked sparse directory and exact canonical conversion.

#include "Scene/WorldRegion.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>

namespace TerraEngine::Scene
{
	namespace
	{
		[[nodiscard]] bool CheckedMultiply(std::int64_t left, std::int64_t right, std::int64_t& result)
		{
			if (left > 0 && left > std::numeric_limits<std::int64_t>::max() / right) return false;
			if (left < 0 && left < std::numeric_limits<std::int64_t>::min() / right) return false;
			result = left * right;
			return true;
		}

		[[nodiscard]] bool CheckedAdd(std::int64_t left, std::int64_t right, std::int64_t& result)
		{
			if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
				|| (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) return false;
			result = left + right;
			return true;
		}

		[[nodiscard]] bool AxisToRegion(std::int64_t cell, double offset, std::int64_t& region)
		{
			std::int64_t cellRegions = 0;
			if (!CheckedMultiply(cell, kWorldRegionsPerCellAxis, cellRegions)) return false;
			// Canonical cells are centred on their exact cell coordinate. Region zero
			// therefore begins at world metre zero rather than at the negative half of
			// cell zero; signed floor preserves the expected negative region keys.
			const std::int64_t localRegion = static_cast<std::int64_t>(std::floor(offset / kWorldRegionEdgeMetres));
			return localRegion >= -(kWorldRegionsPerCellAxis / 2)
				&& localRegion < (kWorldRegionsPerCellAxis / 2)
				&& CheckedAdd(cellRegions, localRegion, region);
		}
	}

	std::size_t WorldRegionKeyHash::operator()(const WorldRegionKey& key) const noexcept
	{
		std::size_t seed = std::hash<std::int64_t>{}(key.x);
		seed ^= std::hash<std::int64_t>{}(key.y) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
		return seed ^ (std::hash<std::int64_t>{}(key.z) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
	}

	WorldCoordinateError WorldPositionToRegionKey(const WorldPosition& position, WorldRegionKey& key)
	{
		const WorldCoordinateResult normalized = NormalizeWorldPosition(position);
		if (!normalized.Succeeded()) return normalized.error;
		if (!AxisToRegion(normalized.position.cell.x, normalized.position.offsetMetres.x, key.x)
			|| !AxisToRegion(normalized.position.cell.y, normalized.position.offsetMetres.y, key.y)
			|| !AxisToRegion(normalized.position.cell.z, normalized.position.offsetMetres.z, key.z)) return WorldCoordinateError::CellOverflow;
		return WorldCoordinateError::None;
	}

	WorldPosition WorldRegionCanonicalOrigin(WorldRegionKey key)
	{
		// Choose the canonical cell whose region remainder is in [-4, 3]. This is
		// the exact inverse of AxisToRegion and keeps the offset normalized.
		auto split = [](std::int64_t value, std::int64_t& cell, std::int64_t& local) {
			cell = value / kWorldRegionsPerCellAxis;
			local = value % kWorldRegionsPerCellAxis;
			if (local >= kWorldRegionsPerCellAxis / 2) { local -= kWorldRegionsPerCellAxis; ++cell; }
			else if (local < -(kWorldRegionsPerCellAxis / 2)) { local += kWorldRegionsPerCellAxis; --cell; }
		};
		WorldPosition result{};
		std::int64_t localX = 0, localY = 0, localZ = 0;
		split(key.x, result.cell.x, localX); split(key.y, result.cell.y, localY); split(key.z, result.cell.z, localZ);
		result.offsetMetres = glm::dvec3(static_cast<double>(localX), static_cast<double>(localY), static_cast<double>(localZ)) * kWorldRegionEdgeMetres;
		return result;
	}

	bool IsValidWorldRegionTransition(WorldRegionState from, WorldRegionState to)
	{
		if (from == to) return true;
		switch (from)
		{
		case WorldRegionState::Unloaded: return to == WorldRegionState::Requested;
		case WorldRegionState::Requested: return to == WorldRegionState::Loading || to == WorldRegionState::Failed || to == WorldRegionState::Unloaded;
		case WorldRegionState::Loading: return to == WorldRegionState::Prepared || to == WorldRegionState::Failed;
		case WorldRegionState::Prepared: return to == WorldRegionState::Resident || to == WorldRegionState::Failed || to == WorldRegionState::Unloaded;
		case WorldRegionState::Resident: return to == WorldRegionState::EvictionPending || to == WorldRegionState::Failed;
		case WorldRegionState::EvictionPending: return to == WorldRegionState::Unloaded || to == WorldRegionState::Resident || to == WorldRegionState::Failed;
		case WorldRegionState::Failed: return to == WorldRegionState::Requested || to == WorldRegionState::Unloaded;
		}
		return false;
	}

	std::string_view ToString(WorldRegionState state)
	{
		switch (state) { case WorldRegionState::Unloaded: return "Unloaded"; case WorldRegionState::Requested: return "Requested"; case WorldRegionState::Loading: return "Loading"; case WorldRegionState::Prepared: return "Prepared"; case WorldRegionState::Resident: return "Resident"; case WorldRegionState::EvictionPending: return "EvictionPending"; case WorldRegionState::Failed: return "Failed"; }
		return "Unknown";
	}

	bool IsWorldRegionEntryValid(const WorldRegionDirectoryEntry& entry)
	{
		const bool contained = std::none_of(entry.artifactPath.begin(), entry.artifactPath.end(), [](const auto& component) { return component == ".."; });
		const bool hashIsHex = std::all_of(entry.contentHash.begin(), entry.contentHash.end(), [](unsigned char value) { return std::isxdigit(value) != 0; });
		return !entry.artifactPath.empty() && !entry.artifactPath.is_absolute() && contained
			&& entry.revision != 0 && entry.contentHash.size() == 64U && hashIsHex;
	}

	nlohmann::json SerializeWorldRegionArtifactManifest(const WorldRegionArtifactManifest& manifest)
	{
		return {{"format", kTerraRegionUnboundedFormat}, {"key", {{"x", std::to_string(manifest.key.x)}, {"y", std::to_string(manifest.key.y)}, {"z", std::to_string(manifest.key.z)}}}, {"contentHash", manifest.contentHash}, {"sourceGeneration", manifest.sourceGeneration}};
	}

	std::optional<WorldRegionArtifactManifest> DeserializeWorldRegionArtifactManifest(const nlohmann::json& value, std::string* outError)
	{
		auto fail = [outError](std::string reason) -> std::optional<WorldRegionArtifactManifest> { if (outError) *outError = std::move(reason); return std::nullopt; };
		if (!value.is_object() || value.value("format", std::string{}) != kTerraRegionUnboundedFormat || !value.contains("key") || !value["key"].is_object()) return fail("region format must be TerraRegionUnboundedV1");
		WorldRegionArtifactManifest result;
		auto parseAxis = [&value](std::string_view axis, std::int64_t& target) {
			const auto found = value["key"].find(std::string(axis));
			if (found == value["key"].end() || !found->is_string()) return false;
			const std::string text = found->get<std::string>();
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), target);
			return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
		};
		if (!parseAxis("x", result.key.x) || !parseAxis("y", result.key.y) || !parseAxis("z", result.key.z)) return fail("region keys must use exact decimal int64 strings");
		if (!value.contains("contentHash") || !value["contentHash"].is_string() || !value.contains("sourceGeneration") || !value["sourceGeneration"].is_number_unsigned()) return fail("region manifest hash or source generation is invalid");
		result.contentHash = value["contentHash"].get<std::string>(); result.sourceGeneration = value["sourceGeneration"].get<std::uint64_t>();
		if (result.sourceGeneration == 0U || result.contentHash.size() != 64U || !std::all_of(result.contentHash.begin(), result.contentHash.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return fail("region manifest must carry a generation and SHA-256 hex hash");
		return result;
	}

	bool WorldRegionDirectory::Upsert(WorldRegionDirectoryEntry entry)
	{
		if (!IsWorldRegionEntryValid(entry)) return false;
		std::unique_lock lock(mutex);
		const auto existing = entries.find(entry.key);
		if (existing != entries.end() && entry.revision < existing->second.revision) return false;
		entries[entry.key] = std::move(entry);
		return true;
	}

	bool WorldRegionDirectory::SetHomeRegion(const TerraEngine::UUID& objectId, WorldRegionKey key)
	{
		if (objectId == TerraEngine::EMPTY_UUID) return false;
		std::unique_lock lock(mutex);
		if (!entries.contains(key)) return false;
		homeRegions[objectId] = key;
		return true;
	}

	bool WorldRegionDirectory::Transition(WorldRegionKey key, WorldRegionState next, std::string failure)
	{
		std::unique_lock lock(mutex);
		auto entry = entries.find(key);
		if (entry == entries.end() || !IsValidWorldRegionTransition(entry->second.state, next)) return false;
		entry->second.state = next;
		entry->second.lastFailure = next == WorldRegionState::Failed ? std::move(failure) : std::string{};
		return true;
	}

	bool WorldRegionDirectory::AcquireLease(WorldRegionKey key, WorldRegionLease lease)
	{
		if (lease.leaseId.empty() || lease.generation == 0U) return false;
		std::unique_lock lock(mutex);
		auto entry = entries.find(key);
		if (entry == entries.end()) return false;
		auto existing = std::find_if(entry->second.leases.begin(), entry->second.leases.end(), [&lease](const WorldRegionLease& value) { return value.leaseId == lease.leaseId; });
		if (existing != entry->second.leases.end() && existing->generation > lease.generation) return false;
		if (existing == entry->second.leases.end()) entry->second.leases.push_back(std::move(lease)); else *existing = std::move(lease);
		return true;
	}

	bool WorldRegionDirectory::ReleaseLease(WorldRegionKey key, std::string_view leaseId, std::uint64_t generation)
	{
		std::unique_lock lock(mutex);
		auto entry = entries.find(key);
		if (entry == entries.end()) return false;
		auto lease = std::find_if(entry->second.leases.begin(), entry->second.leases.end(), [leaseId](const WorldRegionLease& value) { return value.leaseId == leaseId; });
		if (lease == entry->second.leases.end() || lease->generation != generation) return false;
		entry->second.leases.erase(lease);
		return true;
	}

	std::optional<WorldRegionDirectoryEntry> WorldRegionDirectory::Find(WorldRegionKey key) const { std::shared_lock lock(mutex); const auto entry = entries.find(key); return entry == entries.end() ? std::nullopt : std::optional<WorldRegionDirectoryEntry>(entry->second); }
	std::optional<WorldRegionKey> WorldRegionDirectory::FindHomeRegion(const TerraEngine::UUID& objectId) const { std::shared_lock lock(mutex); const auto found = homeRegions.find(objectId); return found == homeRegions.end() ? std::nullopt : std::optional<WorldRegionKey>(found->second); }
	std::vector<WorldRegionDirectoryEntry> WorldRegionDirectory::Entries() const { std::shared_lock lock(mutex); std::vector<WorldRegionDirectoryEntry> result; result.reserve(entries.size()); for (const auto& [key, entry] : entries) { (void)key; result.push_back(entry); } std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.key < right.key; }); return result; }
	std::size_t WorldRegionDirectory::Size() const { std::shared_lock lock(mutex); return entries.size(); }
}
