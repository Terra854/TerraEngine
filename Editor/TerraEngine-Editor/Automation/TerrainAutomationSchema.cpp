// TerrainAutomationSchema.cpp -- Strict lowering from JSON into typed bounds.

#include "TerrainAutomationSchema.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		constexpr std::size_t kMaximumAutomationAreas = 4096u;
		constexpr std::size_t kMaximumAutomationSamples = kMaximumAutomationAreas * TerraEngine::Terrain::kTerrainAreaSampleCount;
		constexpr std::size_t kMaximumAutomationBytes = 512u * 1024u * 1024u;

		TerrainAutomationValidation Fail(std::string category, std::string message)
		{
			return {false, std::move(category), std::move(message), {}};
		}

		std::optional<std::int64_t> ParseI64(const nlohmann::json& value)
		{
			if(!value.is_string()) return std::nullopt;
			const std::string text = value.get<std::string>();
			std::int64_t result = 0;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional{result} : std::nullopt;
		}

		std::optional<std::uint64_t> ParseU64(const nlohmann::json& value)
		{
			if(!value.is_string()) return std::nullopt;
			const std::string text = value.get<std::string>();
			std::uint64_t result = 0u;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional{result} : std::nullopt;
		}

		bool IsCanonicalHash(std::string_view value)
		{
			return value.size() == 32u && std::all_of(value.begin(), value.end(), [](char character) {
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			});
		}

		std::optional<TerraEngine::Terrain::ContentHash> ParseHash(std::string_view text)
		{
			if(!IsCanonicalHash(text)) return std::nullopt;
			auto half = [text](std::size_t offset) -> std::optional<std::uint64_t> {
				std::uint64_t result = 0u;
				const auto parsed = std::from_chars(text.data() + offset, text.data() + offset + 16u, result, 16);
				return parsed.ec == std::errc{} && parsed.ptr == text.data() + offset + 16u ? std::optional{result} : std::nullopt;
			};
			const auto high = half(0u);
			const auto low = half(16u);
			return high && low ? std::optional<TerraEngine::Terrain::ContentHash>{{*low, *high}} : std::nullopt;
		}

		std::optional<TerraEngine::UUID> ParseUuid(const nlohmann::json& value)
		{
			if(!value.is_string()) return std::nullopt;
			const std::string text = value.get<std::string>();
			if(text.size() != 36u || text[8u] != '-' || text[13u] != '-' || text[18u] != '-' || text[23u] != '-') return std::nullopt;
			std::string compact;
			compact.reserve(32u);
			for(char character : text) if(character != '-') compact.push_back(character);
			return TerraEngine::ParseUUID(compact);
		}

		bool HasOnlyKeys(const nlohmann::json& value, std::initializer_list<std::string_view> allowed)
		{
			if(!value.is_object()) return false;
			for(auto entry = value.begin(); entry != value.end(); ++entry)
				if(std::find(allowed.begin(), allowed.end(), entry.key()) == allowed.end()) return false;
			return true;
		}

		std::optional<TerraEngine::Scene::WorldPosition> ParseWorldPosition(const nlohmann::json& value)
		{
			if(!HasOnlyKeys(value, {"cellX", "cellY", "cellZ", "localX", "localY", "localZ"}) || value.size() != 6u) return std::nullopt;
			const auto cellX = ParseI64(value["cellX"]);
			const auto cellY = ParseI64(value["cellY"]);
			const auto cellZ = ParseI64(value["cellZ"]);
			if(!cellX || !cellY || !cellZ) return std::nullopt;
			TerraEngine::Scene::WorldPosition result{};
			result.cell = {*cellX, *cellY, *cellZ};
			for(const char* key : {"localX", "localY", "localZ"}) if(!value[key].is_number()) return std::nullopt;
			result.offsetMetres = {value["localX"].get<double>(), value["localY"].get<double>(), value["localZ"].get<double>()};
			for(double offset : {result.offsetMetres.x, result.offsetMetres.y, result.offsetMetres.z})
				if(!std::isfinite(offset) || offset < -TerraEngine::Scene::kWorldCellHalfEdgeMetres || offset >= TerraEngine::Scene::kWorldCellHalfEdgeMetres) return std::nullopt;
			return result;
		}

		std::optional<std::int64_t> MetreSample(std::int64_t cell, double local)
		{
			// The active terrain authoring store currently uses the v1 one-metre
			// lattice. Preserve the full canonical world cell before checked lowering.
			const long double value = static_cast<long double>(cell) * TerraEngine::Scene::kWorldCellEdgeMetres + static_cast<long double>(local);
			const long double sample = std::floor(value);
			if(sample < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) || sample > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
			return static_cast<std::int64_t>(sample);
		}
	}

	TerrainAutomationValidation ParseTerrainAutomationRequest(const nlohmann::json& json)
	{
		if(!HasOnlyKeys(json, {"schemaVersion", "transactionId", "correlationId", "idempotencyKey", "target", "operation", "worldBounds", "expectedRootHash", "expectedLayerStackGeneration", "observations", "budgets", "previewOnly", "approvalFingerprint"})) return Fail("InvalidSchema", "Terrain request contains an unknown top-level property.");
		if(json.value("schemaVersion", 0) != 1 || !json.contains("transactionId") || !json.contains("correlationId") || !json.contains("idempotencyKey") || !json.contains("target") || !json.contains("operation") || !json.contains("worldBounds") || !json.contains("expectedRootHash") || !json.contains("observations") || !json.contains("budgets") || !json.contains("previewOnly")) return Fail("InvalidSchema", "terrain-edit-v1 requires every canonical identity, operation, observation, budget, and preview field.");

		TerrainAutomationRequest request{};
		const auto transactionId = ParseUuid(json["transactionId"]);
		if(!HasOnlyKeys(json["target"], {"assetUuid", "instanceUuid"}) || json["target"].size() != 2u) return Fail("InvalidSchema", "Terrain target requires only asset and instance UUIDs.");
		const auto assetId = ParseUuid(json["target"]["assetUuid"]);
		const auto instanceId = ParseUuid(json["target"]["instanceUuid"]);
		if(!transactionId || !assetId || !instanceId) return Fail("InvalidSchema", "Transaction, asset, and instance UUIDs must use canonical hyphenated form.");
		request.transactionId = *transactionId;
		request.assetId = *assetId;
		request.instanceId = *instanceId;
		if(!json["correlationId"].is_string() || !json["idempotencyKey"].is_string()) return Fail("InvalidSchema", "Correlation and idempotency identities must be strings.");
		request.correlationId = json["correlationId"].get<std::string>();
		request.idempotencyKey = json["idempotencyKey"].get<std::string>();
		if(request.correlationId.empty() || request.correlationId.size() > 128u || request.idempotencyKey.empty() || request.idempotencyKey.size() > 128u) return Fail("InvalidSchema", "Correlation and idempotency identities must contain 1 through 128 characters.");
		if(!json["expectedRootHash"].is_string() || !IsCanonicalHash(json["expectedRootHash"].get<std::string>())) return Fail("InvalidSchema", "Expected root identity must be a lowercase 128-bit hash.");
		request.expectedRootHash = json["expectedRootHash"].get<std::string>();

		if(!HasOnlyKeys(json["operation"], {"kind", "settings", "seed"}) || !json["operation"].contains("kind") || !json["operation"].contains("settings") || !json["operation"]["kind"].is_string() || !json["operation"]["settings"].is_object()) return Fail("InvalidSchema", "Operation requires only kind, settings, and an optional non-negative seed.");
		request.operation = json["operation"]["kind"].get<std::string>();
		static constexpr std::array<std::string_view, 18u> kOperations = {"raise", "lower", "smooth", "flatten", "terrace", "ramp", "stamp", "noise", "paint", "erase-layer", "fill-layer", "normalize-layers", "paint-hole", "erase-hole", "height-remap", "hydraulic-erosion", "thermal-erosion", "spline-grade"};
		if(std::find(kOperations.begin(), kOperations.end(), request.operation) == kOperations.end()) return Fail("UnsupportedOperation", "Terrain operation kind is not part of terrain-edit-v1.");
		request.settings = json["operation"]["settings"];
		if(json["operation"].contains("seed"))
		{
			if(!json["operation"]["seed"].is_number_unsigned()) return Fail("InvalidSchema", "Operation seed must be a non-negative integer.");
			request.seed = json["operation"]["seed"].get<std::uint64_t>();
		}

		if(!HasOnlyKeys(json["worldBounds"], {"min", "max"}) || json["worldBounds"].size() != 2u) return Fail("AmbiguousCoordinates", "Canonical world bounds require only min and max positions.");
		const auto minimum = ParseWorldPosition(json["worldBounds"]["min"]);
		const auto maximum = ParseWorldPosition(json["worldBounds"]["max"]);
		if(!minimum || !maximum) return Fail("AmbiguousCoordinates", "World bounds require finite normalized canonical positions.");
		request.minimumWorld = *minimum;
		request.maximumWorld = *maximum;
		const auto minX = MetreSample(minimum->cell.x, minimum->offsetMetres.x);
		const auto minZ = MetreSample(minimum->cell.z, minimum->offsetMetres.z);
		const auto maxX = MetreSample(maximum->cell.x, maximum->offsetMetres.x);
		const auto maxZ = MetreSample(maximum->cell.z, maximum->offsetMetres.z);
		if(!minX || !minZ || !maxX || !maxZ) return Fail("OutOfBounds", "Canonical world bounds overflow terrain sample addressing.");
		request.bounds = {*minX, *minZ, *maxX, *maxZ};
		if(!request.bounds.IsValid()) return Fail("OutOfBounds", "Terrain bounds must be ordered, non-empty half-open extents.");

		if(!HasOnlyKeys(json["budgets"], {"maxAreas", "maxSamples", "maxPreparedBytes", "workloadClass"}) || json["budgets"].size() != 4u) return Fail("InvalidSchema", "Terrain budgets require area, sample, byte, and workload limits.");
		for(const char* key : {"maxAreas", "maxSamples", "maxPreparedBytes"}) if(!json["budgets"][key].is_number_unsigned()) return Fail("InvalidSchema", "Terrain numeric budgets must be unsigned integers.");
		const std::uint64_t areas = json["budgets"]["maxAreas"].get<std::uint64_t>();
		const std::uint64_t samples = json["budgets"]["maxSamples"].get<std::uint64_t>();
		const std::uint64_t bytes = json["budgets"]["maxPreparedBytes"].get<std::uint64_t>();
		if(areas == 0u || samples == 0u || bytes == 0u || areas > kMaximumAutomationAreas || samples > kMaximumAutomationSamples || bytes > kMaximumAutomationBytes) return Fail("BudgetExceeded", "Terrain automation budgets are zero or exceed the bounded editor contract.");
		if(!json["budgets"]["workloadClass"].is_string()) return Fail("InvalidSchema", "Terrain workload class must be a string.");
		request.workloadClass = json["budgets"]["workloadClass"].get<std::string>();
		if(request.workloadClass != "interactive" && request.workloadClass != "bounded" && request.workloadClass != "large") return Fail("InvalidSchema", "Terrain workload class must be interactive, bounded, or large.");
		request.budget = {static_cast<std::size_t>(areas), static_cast<std::size_t>(samples), static_cast<std::size_t>(bytes)};

		if(!json["observations"].is_array()) return Fail("InvalidSchema", "Terrain observations must be an array.");
		std::set<TerraEngine::Terrain::TerrainAreaCoordinate> observed;
		for(const auto& observation : json["observations"])
		{
			if(!HasOnlyKeys(observation, {"areaX", "areaZ", "editGeneration", "authoredContentHash"}) || observation.size() != 4u) return Fail("InvalidSchema", "Each area observation requires only coordinate, generation, and content identity.");
			const auto x = ParseI64(observation["areaX"]);
			const auto z = ParseI64(observation["areaZ"]);
			const auto generation = ParseU64(observation["editGeneration"]);
			const auto hash = observation["authoredContentHash"].is_string() ? ParseHash(observation["authoredContentHash"].get<std::string>()) : std::nullopt;
			if(!x || !z || !generation || *generation == 0u || !hash) return Fail("InvalidSchema", "Area observations require signed coordinates, a non-zero generation, and a lowercase content hash.");
			const TerraEngine::Terrain::TerrainAreaCoordinate coordinate{*x, *z};
			if(!observed.insert(coordinate).second) return Fail("InvalidSchema", "Duplicate area observations are ambiguous.");
			request.observations.push_back({coordinate, *generation, *hash});
		}
		if(request.observations.empty() || request.observations.size() > request.budget.maximumAreas) return Fail("BudgetExceeded", "Observed areas must be non-empty and fit the declared area budget.");
		std::sort(request.observations.begin(), request.observations.end(), [](const auto& left, const auto& right) { return left.coordinate < right.coordinate; });

		if(json.contains("expectedLayerStackGeneration"))
		{
			const auto generation = ParseU64(json["expectedLayerStackGeneration"]);
			if(!generation) return Fail("InvalidSchema", "Layer stack generation must be an unsigned decimal string.");
			request.expectedLayerStackGeneration = *generation;
		}
		if(!json["previewOnly"].is_boolean()) return Fail("InvalidSchema", "previewOnly must be a boolean.");
		request.previewOnly = json["previewOnly"].get<bool>();
		if(json.contains("approvalFingerprint") && !json["approvalFingerprint"].is_null())
		{
			if(!json["approvalFingerprint"].is_string() || !IsCanonicalHash(json["approvalFingerprint"].get<std::string>())) return Fail("InvalidSchema", "Approval fingerprint must be null or a lowercase 128-bit hash.");
			request.approvalFingerprint = json["approvalFingerprint"].get<std::string>();
		}
		return {true, {}, {}, std::move(request)};
	}

	std::string TerrainApprovalFingerprint(const TerrainAutomationRequest& request)
	{
		std::string source = request.operation + '|' + request.settings.dump() + '|' + request.expectedRootHash + '|'
			+ TerraEngine::ToHexString(request.assetId) + '|' + TerraEngine::ToHexString(request.instanceId) + '|'
			+ std::to_string(request.bounds.minX) + '|' + std::to_string(request.bounds.minZ) + '|'
			+ std::to_string(request.bounds.maxXExclusive) + '|' + std::to_string(request.bounds.maxZExclusive) + '|'
			+ std::to_string(request.budget.maximumAreas) + '|' + std::to_string(request.budget.maximumSamples) + '|'
			+ std::to_string(request.budget.maximumPreparedBytes) + '|' + request.workloadClass + '|'
			+ std::to_string(request.expectedLayerStackGeneration) + '|' + (request.seed ? std::to_string(*request.seed) : "-");
		for(const auto& observation : request.observations)
		{
			source += '|' + std::to_string(observation.coordinate.x) + ',' + std::to_string(observation.coordinate.z) + ','
				+ std::to_string(observation.editGeneration) + ',' + TerraEngine::Terrain::ToHexString(observation.authoredContentHash);
		}
		const auto hash = TerraEngine::Terrain::HashTerrainBytes({reinterpret_cast<const std::uint8_t*>(source.data()), source.size()});
		return TerraEngine::Terrain::ToHexString(hash);
	}
}
