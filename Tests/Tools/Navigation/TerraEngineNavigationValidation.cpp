// TerraEngineNavigationValidation.cpp - real engine navigation evidence runner.

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/Integration/NavigationSceneBootstrap.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Persistence/SavedNavigationCache.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "Scene/WorldStreamingSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <span>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	bool WriteProviderCacheFixture(std::string_view mode, const std::filesystem::path& output)
	{
		if((mode != "current" && mode != "stale") || !output.is_absolute()) return false;
		// Usage: TerraEngineNavigationAcceptance --write-cache-fixture current|stale
		// <absolute-output.tenav>. This emits a standalone provider round-trip
		// diagnostic, not a cache claimed to belong to the 129x129 workload.
		using namespace TerraEngine::Navigation;
		NavigationGeometryDocument document{};
		document.kind = NavigationGeometryPayloadKind::TriangleMesh;
		document.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F}, {8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F}};
		document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
		NavigationGeometrySnapshotDescriptor sourceDescriptor{};
		sourceDescriptor.sourceRevision = 1U;
		sourceDescriptor.canonicalPlacementRevision = 1U;
		sourceDescriptor.physicalReadyRevision = 1U;
		sourceDescriptor.sourceKind = NavigationSourceKind::Terrain;
		const auto snapshot = NavigationGeometrySnapshot::Create(sourceDescriptor, document);
		if(!snapshot.accepted || !snapshot.snapshot) return false;
		RecastGroundBuildInput buildInput{};
		buildInput.profile = NavigationProfile::Defaults();
		buildInput.sources = {snapshot.snapshot};
		buildInput.maximumTiles = 4U;
		const RecastGroundBuildResult built = BuildRecastGroundTiles(buildInput);
		if(!built.Succeeded() || built.tiles.empty() || built.tiles.front().pristineBytes.empty()) return false;
		const GroundCoverageEvidence evidence = CaptureGroundCoverageEvidence(buildInput);
		const SavedGroundTileExportResult chunks = BuildSavedNavigationChunks(built.tiles);
		if(!evidence.accepted || !chunks.accepted || chunks.chunks.empty()) return false;
		SavedNavigationDescriptor descriptor{};
		descriptor.formatVersion = kSavedNavigationFormatVersion;
		descriptor.backendAbi = std::string(kSavedNavigationBackendAbi);
		descriptor.sourceFingerprint = evidence.sourceFingerprint;
		if(descriptor.sourceFingerprint == 0U) return false;
		descriptor.settingsFingerprint = evidence.settingsFingerprint;
		const auto write = WriteSavedNavigationCacheAtomically(output, descriptor,
			chunks.chunks);
		if(!write.accepted) return false;
		const std::uint64_t expectedSettings = mode == "stale" ? descriptor.settingsFingerprint + 1U : descriptor.settingsFingerprint;
		SavedNavigationDescriptor expected = descriptor;
		expected.settingsFingerprint = expectedSettings;
		const SavedNavigationReadResult readBack = ReadSavedNavigationCache(output, expected);
		if((mode == "current" && !readBack.accepted) || (mode == "stale" && readBack.accepted)) return false;
		nlohmann::json state = {{"schemaVersion", 1}, {"fixture", "provider-generated-recast-tile"},
			{"mode", std::string(mode)}, {"storedSettingsFingerprint", descriptor.settingsFingerprint},
			{"expectedSettingsFingerprint", expectedSettings}, {"tileBytes", chunks.chunks.front().bytes.size()},
			{"readBackAccepted", readBack.accepted}, {"readBackReason", readBack.reason},
			{"runtimeTileConsumptionProven", false}};
		std::ofstream metadata(output.string() + ".json", std::ios::binary | std::ios::trunc);
		if(!metadata) return false;
		metadata << state.dump(2) << '\n';
		return metadata.good();
	}

	struct Options
	{
		std::string lane = "rendered";
		std::filesystem::path manifest;
		std::vector<std::string> matrices{"semantic"};
		std::filesystem::path output = "navigation-evidence.json";
		// The reviewed reference workload always uses these defaults. Smoke runs
		// may shorten only the duration, never silently reduce the agent count.
		std::uint32_t agents = 256U;
		std::uint32_t requestsPerSecond = 100U;
		std::uint32_t durationSeconds = 1U;
		std::uint32_t cycles = 100U;
		std::uint32_t idlePairs = 5U;
		// The longest qualification soak records 180,000 route outcomes. Retain
		// that complete denominator by default while still enforcing a hard cap.
		std::uint32_t rawSampleLimit = 1'000'000U;
		bool qualification = false;
	};

	bool ParseUnsigned(std::string_view text, std::uint32_t& value)
	{
		try {
			const unsigned long parsed = std::stoul(std::string(text));
			if(parsed == 0U || parsed > std::numeric_limits<std::uint32_t>::max()) return false;
			value = static_cast<std::uint32_t>(parsed);
			return true;
		} catch(...) { return false; }
	}

	bool ParseOptions(int argc, char** argv, Options& options)
	{
		for(int index = 1; index < argc; ++index) {
			const std::string_view argument(argv[index]);
			if(argument == "--qualification") {
				options.qualification = true;
				continue;
			}
			if(index + 1 >= argc) return false;
			const std::string_view value(argv[++index]);
			if(argument == "--lane") options.lane = value;
			else if(argument == "--manifest") options.manifest = std::filesystem::path(value);
			else if(argument == "--matrices") {
				options.matrices.clear();
				std::string matrixList(value);
				std::size_t begin = 0U;
				while(begin <= matrixList.size()) {
					const std::size_t end = matrixList.find(',', begin);
					const std::string matrix = matrixList.substr(begin, end == std::string::npos ? end : end - begin);
					if(matrix.empty()) return false;
					options.matrices.push_back(matrix);
					if(end == std::string::npos) break;
					begin = end + 1U;
				}
			}
			else if(argument == "--mode") options.matrices = {std::string(value)};
			else if(argument == "--output") options.output = std::filesystem::path(value);
			else if(argument == "--agents") { if(!ParseUnsigned(value, options.agents)) return false; }
			else if(argument == "--requests-per-second") { if(!ParseUnsigned(value, options.requestsPerSecond)) return false; }
			else if(argument == "--duration-seconds") { if(!ParseUnsigned(value, options.durationSeconds)) return false; }
			else if(argument == "--cycles") { if(!ParseUnsigned(value, options.cycles)) return false; }
			else if(argument == "--idle-pairs") { if(!ParseUnsigned(value, options.idlePairs)) return false; }
			else if(argument == "--raw-sample-limit") { if(!ParseUnsigned(value, options.rawSampleLimit)) return false; }
			else return false;
		}
		const std::vector<std::string> validMatrices{"semantic", "performance", "idle", "idle-overload", "overload", "soak", "lifecycle", "usability"};
		const bool requiresFiveMinuteMeasurement = std::any_of(options.matrices.begin(), options.matrices.end(), [](const std::string& matrix) {
			return matrix == "performance" || matrix == "idle" || matrix == "idle-overload" || matrix == "overload";
		});
		return (options.lane == "editor" || options.lane == "rendered" || options.lane == "export-rendered"
			|| options.lane == "headless" || options.lane == "export-headless")
			&& !options.matrices.empty()
			&& std::all_of(options.matrices.begin(), options.matrices.end(), [&validMatrices](const std::string& matrix) {
				return std::find(validMatrices.begin(), validMatrices.end(), matrix) != validMatrices.end();
			})
			&& options.agents <= 4096U && options.requestsPerSecond <= 10000U
			&& options.durationSeconds <= 7200U && options.cycles <= 10000U
			&& options.idlePairs <= 100U && options.rawSampleLimit <= 1'000'000U
			&& (!options.qualification || (options.agents == 256U && options.requestsPerSecond == 100U
				&& options.idlePairs == 5U
				&& (!requiresFiveMinuteMeasurement || options.durationSeconds >= 300U)
				&& (std::find(options.matrices.begin(), options.matrices.end(), "soak") == options.matrices.end()
					|| options.durationSeconds >= 1800U)
				&& (std::find(options.matrices.begin(), options.matrices.end(), "lifecycle") == options.matrices.end()
					|| options.cycles >= 100U)));
	}

	// Keep evidence hashing self-contained so installed validation has no
	// dependency on a platform crypto provider or developer environment.
	std::array<std::uint8_t, 32U> Sha256(std::span<const std::uint8_t> input)
	{
		constexpr std::array<std::uint32_t, 64U> constants{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
		std::array<std::uint32_t, 8U> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
			0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
		std::vector<std::uint8_t> padded(input.begin(), input.end());
		padded.push_back(0x80U);
		while((padded.size() % 64U) != 56U) padded.push_back(0U);
		const std::uint64_t bits = static_cast<std::uint64_t>(input.size()) * 8U;
		for(int shift = 56; shift >= 0; shift -= 8) padded.push_back(static_cast<std::uint8_t>(bits >> shift));
		for(std::size_t block = 0U; block < padded.size(); block += 64U) {
			std::array<std::uint32_t, 64U> words{};
			for(std::size_t index = 0U; index < 16U; ++index) {
				const std::size_t offset = block + index * 4U;
				words[index] = (static_cast<std::uint32_t>(padded[offset]) << 24U)
					| (static_cast<std::uint32_t>(padded[offset + 1U]) << 16U)
					| (static_cast<std::uint32_t>(padded[offset + 2U]) << 8U) | padded[offset + 3U];
			}
			for(std::size_t index = 16U; index < words.size(); ++index) {
				const std::uint32_t s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
				const std::uint32_t s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
				words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
			}
			std::uint32_t a = state[0U], b = state[1U], c = state[2U], d = state[3U], e = state[4U], f = state[5U], g = state[6U], h = state[7U];
			for(std::size_t index = 0U; index < words.size(); ++index) {
				const std::uint32_t choose = (e & f) ^ (~e & g);
				const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
				const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
				const std::uint32_t t1 = h + s1 + choose + constants[index] + words[index];
				const std::uint32_t t2 = s0 + majority;
				h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			state[0U] += a; state[1U] += b; state[2U] += c; state[3U] += d;
			state[4U] += e; state[5U] += f; state[6U] += g; state[7U] += h;
		}
		std::array<std::uint8_t, 32U> digest{};
		for(std::size_t index = 0U; index < state.size(); ++index) {
			digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
			digest[index * 4U + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
			digest[index * 4U + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
			digest[index * 4U + 3U] = static_cast<std::uint8_t>(state[index]);
		}
		return digest;
	}

	std::string Sha256Hex(std::span<const std::uint8_t> bytes)
	{
		const auto digest = Sha256(bytes);
		std::ostringstream result;
		result << std::hex << std::setfill('0');
		for(const std::uint8_t byte : digest) result << std::setw(2) << static_cast<unsigned int>(byte);
		return result.str();
	}

	std::string Sha256Hex(std::string_view text)
	{
		// String identities use the same byte-exact UTF-8/ASCII representation as
		// the JSON evidence writer, avoiding platform-dependent wchar encodings.
		return Sha256Hex(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
	}

	std::string HashFile(const std::filesystem::path& path)
	{
		if(path.empty() || !std::filesystem::is_regular_file(path)) return {};
		std::ifstream input(path, std::ios::binary);
		if(!input) return {};
		const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		return Sha256Hex(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
	}

	std::uint64_t Percentile(std::vector<std::uint64_t> values, double percentile)
	{
		if(values.empty()) return 0U;
		std::sort(values.begin(), values.end());
		const std::size_t index = static_cast<std::size_t>(
			percentile * static_cast<double>(values.size() - 1U));
		return values[index];
	}

	TerraEngine::Navigation::NavigationLocation Location(
		TerraEngine::Navigation::NavigationWorldHandle world, double x)
	{
		// Keep acceptance endpoints one metre inside the captured terrain. Tile and
		// vertical-band boundaries are independent missing-neighbour scenarios, not
		// the ordinary-object route this installed contract intends to measure.
		return {world, {{0, 0, 0}, {x, 0.0, 1.0}}};
	}

	std::string_view DynamicOperationName(TerraEngine::Scene::NavigationSourceMutationOperation operation)
	{
		using Operation = TerraEngine::Scene::NavigationSourceMutationOperation;
		switch(operation) {
		case Operation::Create: return "create";
		case Operation::Enable: return "enable";
		case Operation::Disable: return "disable";
		case Operation::Move: return "move";
		case Operation::Rotate: return "rotate";
		case Operation::Scale: return "scale";
		case Operation::Delete: return "delete";
		case Operation::Replace: return "replace";
		case Operation::Reshape: return "reshape";
		}
		return "unknown";
	}

	// The installed manifest is a reviewed semantic contract, not merely an input
	// to hash. Parse its required scenario groups before running any workload so a
	// truncated or unrelated installed fixture cannot become passing evidence.
	struct ManifestContract
	{
		bool parsed = false;
		bool semanticCasesValid = false;
		bool dynamicCasesValid = false;
		bool streamingCasesValid = false;
		bool lifecycleCasesValid = false;
		std::size_t fixtureCaseCount = 0U;
		std::size_t expandedCaseCount = 0U;
		nlohmann::json document;
	};

	ManifestContract ParseManifestContract(const std::filesystem::path& path)
	{
		ManifestContract result{};
		try {
			std::ifstream input(path, std::ios::binary);
			if(!input) return result;
			input >> result.document;
			result.parsed = result.document.value("schemaVersion", 0U) == 1U
				&& result.document.value("fixtureFormat", std::string{}) == "TerraEngineGroundNavigationV1";
			if(!result.parsed) return result;
			const nlohmann::json& fixtures = result.document.at("fixtures");
			const nlohmann::json& cases = result.document.at("cases");
			const nlohmann::json& dynamic = result.document.at("dynamicGeometry");
			const nlohmann::json& streaming = result.document.at("streamingLargeWorld");
			const nlohmann::json& lifecycle = result.document.at("lifecycle");
			const nlohmann::json& expansion = result.document.at("expansion");
			result.fixtureCaseCount = cases.size();
			result.expandedCaseCount = expansion.value("totalStableCaseCount", 0U);
			bool hasAutomaticTerrain = false;
			bool hasAutomaticGeometry = false;
			for(const nlohmann::json& fixture : fixtures) {
				if(!fixture.contains("id") || !fixture.contains("profiles") || !fixture.contains("sources")) continue;
				for(const nlohmann::json& source : fixture.at("sources")) {
					hasAutomaticTerrain = hasAutomaticTerrain || source == "terrain-height-triangles-v1";
					hasAutomaticGeometry = hasAutomaticGeometry || source == "automatic-geometry-v1";
				}
			}
			result.semanticCasesValid = fixtures.size() >= 2U && cases.size() >= 25U
				&& result.expandedCaseCount >= 100U && hasAutomaticTerrain && hasAutomaticGeometry;
			result.dynamicCasesValid = dynamic.value("revisionCount", 0U) >= 9U
				&& dynamic.contains("revisions") && dynamic.at("revisions").size() >= 9U;
			result.streamingCasesValid = streaming.contains("cases") && streaming.at("cases").size() >= 4U
				&& streaming.contains("coverageSequences") && streaming.at("coverageSequences").size() >= 4U;
			result.lifecycleCasesValid = lifecycle.contains("cases") && lifecycle.at("cases").size() >= 2U
				&& lifecycle.contains("publicationSequence") && lifecycle.at("publicationSequence").size() >= 5U;
		} catch(const std::exception&) {
			return ManifestContract{};
		}
		return result;
	}

	TerraEngine::Navigation::NavigationEligibleSourceCandidate MakeAutomaticCandidate(
		std::string_view sourceId, TerraEngine::Navigation::NavigationSourceKind kind)
	{
		using namespace TerraEngine::Navigation;
		NavigationEligibleSourceCandidate candidate{};
		candidate.identity.sourceId = NavigationUuid::FromString(sourceId).value;
		candidate.identity.objectId = NavigationUuid::FromString("a1f77899-1ef3-4c1a-8a32-f4a0f96077c3").value;
		candidate.identity.objectLifetimeGeneration = 1U;
		candidate.identity.assetId = NavigationUuid::FromString("eae00c9d-3515-4c9a-abdb-1a6df3cb4f04").value;
		candidate.identity.kind = kind;
		candidate.revisions = {1U, 2U, 3U, 4U, 5U};
		candidate.activeInHierarchy = true;
		candidate.physicalBackingCurrent = true;
		return candidate;
	}

	TerraEngine::Navigation::NavigationBootstrapSourceInput MakeAutomaticTerrainSource()
	{
		using namespace TerraEngine::Navigation;
		NavigationBootstrapSourceInput source{};
		source.candidate = MakeAutomaticCandidate("4e4a0ddf-2c35-4d75-8cb8-fa0b6e1061d8", NavigationSourceKind::Terrain);
		TerrainNavigationPublishedSamples terrain{};
		terrain.terrainPresent = true;
		terrain.descriptor.sourceUuid.value = source.candidate.identity.sourceId;
		terrain.descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(source.candidate.identity, source.candidate.revisions);
		terrain.descriptor.canonicalPlacementRevision = source.candidate.revisions.transformRevision;
		terrain.descriptor.physicalReadyRevision = source.candidate.revisions.physicalPublicationGeneration;
		terrain.descriptor.sourceKind = NavigationSourceKind::Terrain;
		terrain.sampleColumns = 2U;
		terrain.sampleRows = 2U;
		terrain.sampleSpacingMetres = 32.0F;
		terrain.heights = {0.0F, 0.0F, 0.0F, 0.0F};
		source.terrain = std::move(terrain);
		return source;
	}

	TerraEngine::Navigation::NavigationBootstrapSourceInput MakeAutomaticMeshSource()
	{
		using namespace TerraEngine::Navigation;
		NavigationBootstrapSourceInput source{};
		source.candidate = MakeAutomaticCandidate("a35d7ef4-fc86-41df-a516-c2d03df2916e", NavigationSourceKind::Mesh);
		NavigationGeometrySnapshotDescriptor descriptor{};
		descriptor.sourceUuid.value = source.candidate.identity.sourceId;
		descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(source.candidate.identity, source.candidate.revisions);
		descriptor.canonicalPlacementRevision = source.candidate.revisions.transformRevision;
		descriptor.physicalReadyRevision = source.candidate.revisions.physicalPublicationGeneration;
		descriptor.sourceKind = NavigationSourceKind::Mesh;
		NavigationGeometryDocument geometry{};
		geometry.kind = NavigationGeometryPayloadKind::TriangleMesh;
		geometry.vertices = {{0.0F, 0.0F, 0.0F}, {32.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 32.0F}};
		geometry.triangles = {{0U, 1U, 2U}};
		source.candidate.geometry = NavigationGeometrySnapshot::Create(descriptor, geometry).snapshot;
		return source;
	}

	TerraEngine::Navigation::NavigationSceneBootstrapCapture CaptureAutomaticBootstrap(bool includeTerrain)
	{
		using namespace TerraEngine::Navigation;
		NavigationSceneBootstrapInput input{};
		if(includeTerrain) input.sources.push_back(MakeAutomaticTerrainSource());
		input.sources.push_back(MakeAutomaticMeshSource());
		return CaptureNavigationSceneBootstrap(input);
	}

	std::shared_ptr<const TerraEngine::Navigation::NavigationGeometrySnapshot> MakeDynamicSnapshot(
		TerraEngine::Navigation::NavigationUuid source, TerraEngine::Navigation::NavigationGeneration revision,
		double shift)
	{
		using namespace TerraEngine::Navigation;
		// Dynamic publication consumes this detached mesh rather than an authoring
		// object. Its revision deliberately matches the mutation receipt so worker
		// preparation can reject a late physical companion without guessing.
		NavigationGeometrySnapshotDescriptor descriptor{};
		descriptor.sourceUuid.value = source.value;
		descriptor.sourceRevision = revision;
		descriptor.canonicalPlacementRevision = revision;
		descriptor.physicalReadyRevision = revision;
		descriptor.sourceKind = NavigationSourceKind::Procedural;
		NavigationGeometryDocument geometry{};
		geometry.kind = NavigationGeometryPayloadKind::TriangleMesh;
		const float left = static_cast<float>(shift - 1.5);
		const float right = static_cast<float>(shift + 2.5);
		geometry.vertices = {{left, 0.0F, -2.0F}, {right, 0.0F, -2.0F}, {left, 0.0F, 2.0F}};
		geometry.triangles = {{0U, 1U, 2U}};
		const NavigationGeometrySnapshotCreateResult created = NavigationGeometrySnapshot::Create(descriptor, geometry);
		return created.accepted ? created.snapshot : nullptr;
	}

	bool StageDynamicMutationGeometry(TerraEngine::Navigation::NavigationSystem& system,
		TerraEngine::Navigation::NavigationUuid source,
		TerraEngine::Scene::NavigationSourceMutationOperation operation,
		TerraEngine::Navigation::NavigationGeneration revision, double shift)
	{
		using namespace TerraEngine::Navigation;
		// Removals have no replacement mesh. Supplying an empty stage makes the
		// worker produce the tombstone-only publication instead of rebuilding an
		// unrelated automatic terrain mesh for the removed tile.
		if(operation == TerraEngine::Scene::NavigationSourceMutationOperation::Disable
			|| operation == TerraEngine::Scene::NavigationSourceMutationOperation::Delete)
			return system.StageDynamicGroundSources(NavigationProfile::Defaults(), {});
		const std::shared_ptr<const NavigationGeometrySnapshot> snapshot = MakeDynamicSnapshot(source, revision, shift);
		return snapshot != nullptr && system.StageDynamicGroundSources(
			NavigationProfile::Defaults(), {snapshot});
	}

	bool QueueAutomaticBootstrap(TerraEngine::Navigation::NavigationSystem& system,
		const TerraEngine::Navigation::NavigationSceneBootstrapCapture& capture)
	{
		using namespace TerraEngine::Navigation;
		if(!capture.accepted) return false;
		std::vector<NavigationSystem::InitialGroundBuildInput> builds;
		builds.reserve(capture.builds.size());
		for(const NavigationBootstrapBuildInput& build : capture.builds) {
			builds.push_back({build.profile, build.sources, build.sourceIndex, build.eligibleSourceIndex,
				build.terrainPublicationGeneration, 0U});
		}
		return system.QueueInitialGroundBuilds(std::move(builds));
	}
}

int main(int argc, char** argv)
{
	if(argc == 4 && std::string_view(argv[1]) == "--write-cache-fixture") {
		const std::filesystem::path output(argv[3]);
		if(!output.is_absolute()) {
			std::cerr << "Cache fixture output must be an absolute path.\n";
			return 2;
		}
		if(!WriteProviderCacheFixture(argv[2], output)) {
			std::cerr << "Could not write provider-generated navigation cache fixture.\n";
			return 2;
		}
		std::cout << "Wrote provider-generated " << argv[2] << " cache fixture: " << output << '\n';
		return 0;
	}
	Options options;
	if(!ParseOptions(argc, argv, options)) {
		std::cerr << "Usage: TerraEngineNavigationAcceptance --lane <editor|export-rendered|export-headless> --manifest <file> --output <file> --matrices <comma-separated> [--duration-seconds N] [--cycles N]\n";
		return 2;
	}

	using namespace TerraEngine::Navigation;
	const ManifestContract manifestContract = ParseManifestContract(options.manifest);
	if(!manifestContract.parsed) {
		std::cerr << "Installed navigation manifest is missing or malformed.\n";
		return 6;
	}
	NavigationSystem& system = NavigationSystem::Instance();
	system.Shutdown();
	system.Initialize();
	// Route availability must come from the same automatic terrain-plus-physical
	// geometry capture boundary used by an installed scene, never a hand-built
	// source that could hide a missing-terrain or eligibility regression.
	const NavigationSceneBootstrapCapture automaticBootstrap = CaptureAutomaticBootstrap(true);
	const bool automaticCapturePassed = automaticBootstrap.accepted && automaticBootstrap.terrainAvailable
		&& automaticBootstrap.eligibleSourceIndex != nullptr
		&& automaticBootstrap.eligibleSourceIndex->Records().size() >= 2U;
	if(!automaticCapturePassed || !QueueAutomaticBootstrap(system, automaticBootstrap)) {
		system.Shutdown();
		return 3;
	}
	system.Update(0.0F);
	const auto root = system.PublishedWorld();
	if(!root || root->coverage.empty()) {
		system.Shutdown();
		return 3;
	}
	const NavigationOwnerHandle owner{root->world, {1U}, 1U};
	const NavigationLocation start = Location(root->world, 0.0);
	const NavigationLocation destination = Location(root->world, 10.0);
	// Each installed lane proves the complete gameplay-facing object path before
	// its throughput workload begins: one object targets another object's copied
	// canonical pose, receives a route, and moves through its waypoints using the
	// same typed transactional boundary available to a character controller.
	ObjectManager& objects = ObjectManager::instance();
	TerraEngine::Scene::SceneMutationQueue& mutations = TerraEngine::Scene::SceneMutationQueue::Instance();
	objects.clear();
	mutations.Clear();
	const Object navigatorObject = objects.createObject("Installed Navigating Object");
	Transform* navigatorTransform = objects.AddComponent<Transform>(navigatorObject);
	const Object targetObject = objects.createObject("Installed Target Object");
	Transform* targetTransform = objects.AddComponent<Transform>(targetObject);
	// Endpoint capture reads canonical object state. Publish fixture positions
	// through the transactional scene boundary rather than assigning Transform
	// fields directly, which would leave the canonical-pose cache stale.
	TerraEngine::Scene::WorldPose navigatorPose{};
	navigatorPose.position = Location(root->world, 1.0).position;
	TerraEngine::Scene::WorldPose targetPose{};
	targetPose.position = destination.position;
	TerraEngine::Scene::TransactionalMutationBuilder initialPlacement(
		"installed-navigation-initial-placement", TerraEngine::Scene::TransactionOrigin::Test,
		"InstalledNavigationAcceptance");
	initialPlacement.SetWorldTransform(objects, objects.getUUID(navigatorObject), navigatorPose);
	initialPlacement.SetWorldTransform(objects, objects.getUUID(targetObject), targetPose);
	mutations.EnqueueTransaction(std::move(initialPlacement).Build());
	const TerraEngine::Scene::SceneMutationBatchResult initialPlacementResult = mutations.Apply(
		objects, "Installed navigation initial placement");
	if(navigatorTransform == nullptr || targetTransform == nullptr
		|| initialPlacementResult.transactionResults.size() != 1U
		|| initialPlacementResult.transactionResults.front().status != TerraEngine::Scene::TransactionStatus::Committed
		|| initialPlacementResult.transactionResults.front().appliedWriteCount != 2U) {
		objects.clear();
		system.Shutdown();
		return 3;
	}
	const auto locationReceipt = system.RequestRoute(objects, objects.getUUID(navigatorObject),
		Location(root->world, 10.0), NavigationProfile::Defaults());
	const auto destinationReceipt = system.RequestRoute(objects, objects.getUUID(navigatorObject),
		objects.getUUID(targetObject), NavigationProfile::Defaults());
	// Preparation is deliberately bounded to one copied route packet per update.
	// Advance enough protected turns to observe both independently admitted public
	// requests rather than mistaking an honest queued record for a failed route.
	for(std::uint32_t frame = 0U; frame < 4U; ++frame) system.Update(0.0F);
	const auto locationRequest = locationReceipt.request.IsValid()
		? system.ReadRequest(locationReceipt.request) : NavigationStoreReadResult<NavigationRequestRecord>{};
	const auto destinationRoute = destinationReceipt.request.IsValid()
		? system.ReadRoute(system.PublishedRouteForRequest(destinationReceipt.request))
		: Ground::GroundRouteQuery::RetainedRoute{};
	const bool objectLocationRoutePassed = locationReceipt.Succeeded() && locationRequest.record.has_value()
		&& locationRequest.record->outcome == NavigationOutcome::Complete;
	const bool objectDestinationRoutePassed = navigatorTransform != nullptr
		&& targetTransform != nullptr && destinationReceipt.Succeeded()
		&& destinationRoute.IsValid() && !destinationRoute.points.empty()
		&& destinationRoute.points.front().position == navigatorTransform->worldPose.position
		&& destinationRoute.points.back().position == targetTransform->worldPose.position;
	bool objectTargetMovementPassed = objectLocationRoutePassed && objectDestinationRoutePassed;
	std::size_t objectTargetMovementSteps = 0U;
	const TerraEngine::UUID navigatorId = objects.getUUID(navigatorObject);
	for(std::size_t pointIndex = 1U; objectTargetMovementPassed && pointIndex < destinationRoute.points.size(); ++pointIndex) {
		navigatorTransform = objects.GetComponent<Transform>(navigatorObject);
		if(navigatorTransform == nullptr) { objectTargetMovementPassed = false; break; }
		TerraEngine::Scene::WorldPose nextPose = navigatorTransform->worldPose;
		nextPose.position = destinationRoute.points[pointIndex].position;
		TerraEngine::Scene::TransactionalMutationBuilder movement(
			"installed-navigation-gameplay-step-" + std::to_string(pointIndex),
			TerraEngine::Scene::TransactionOrigin::Test, "InstalledNavigationGameplayController");
		movement.SetWorldTransform(objects, navigatorId, nextPose);
		mutations.EnqueueTransaction(std::move(movement).Build());
		const TerraEngine::Scene::SceneMutationBatchResult committed = mutations.Apply(
			objects, "Installed navigation gameplay movement");
		objectTargetMovementPassed = committed.transactionResults.size() == 1U
			&& committed.transactionResults.front().status == TerraEngine::Scene::TransactionStatus::Committed
			&& committed.transactionResults.front().appliedWriteCount == 1U;
		if(objectTargetMovementPassed) ++objectTargetMovementSteps;
	}
	navigatorTransform = objects.GetComponent<Transform>(navigatorObject);
	targetTransform = objects.GetComponent<Transform>(targetObject);
	objectTargetMovementPassed = objectTargetMovementPassed && objectTargetMovementSteps > 0U
		&& navigatorTransform != nullptr && targetTransform != nullptr
		&& navigatorTransform->worldPose.position == targetTransform->worldPose.position;
	// Qualification must exercise real ordinary scene objects. Owner IDs alone
	// are not evidence that the public capture path survives a 256-object load.
	const std::uint32_t workloadObjectCount = options.qualification ? 256U
		: std::max<std::uint32_t>(options.agents, 2U);
	std::vector<TerraEngine::UUID> workloadObjectIds;
	workloadObjectIds.reserve(workloadObjectCount);
	for(std::uint32_t index = 0U; index < workloadObjectCount; ++index) {
		const Object object = objects.createObject("Installed Navigation Workload Object " + std::to_string(index));
		Transform* transform = objects.AddComponent<Transform>(object);
		if(transform == nullptr) {
			objects.clear();
			system.Shutdown();
			return 3;
		}
		workloadObjectIds.push_back(objects.getUUID(object));
	}
	TerraEngine::Scene::TransactionalMutationBuilder workloadPlacement(
		"installed-navigation-workload-placement", TerraEngine::Scene::TransactionOrigin::Test,
		"InstalledNavigationAcceptance");
	for(const TerraEngine::UUID& objectId : workloadObjectIds) {
		TerraEngine::Scene::WorldPose pose{};
		pose.position = Location(root->world, 1.0).position;
		workloadPlacement.SetWorldTransform(objects, objectId, pose);
	}
	mutations.EnqueueTransaction(std::move(workloadPlacement).Build());
	const TerraEngine::Scene::SceneMutationBatchResult workloadPlacementResult = mutations.Apply(
		objects, "Installed navigation workload placement");
	if(workloadPlacementResult.transactionResults.size() != 1U
		|| workloadPlacementResult.transactionResults.front().status != TerraEngine::Scene::TransactionStatus::Committed
		|| workloadPlacementResult.transactionResults.front().appliedWriteCount != workloadObjectIds.size()) {
		objects.clear();
		system.Shutdown();
		return 3;
	}
	// Prove the first ordinary workload object can use the identical public route
	// entry point before pacing the full qualification schedule.  Retaining its
	// captured record in evidence makes a future spatial-cache regression
	// diagnosable without replacing a failed request with synthetic metadata.
	const NavigationSystem::ObjectRouteReceipt workloadProbeReceipt = system.RequestRoute(
		objects, workloadObjectIds.front(), destination, NavigationProfile::Defaults());
	NavigationStoreReadResult<NavigationRequestRecord> workloadProbeRead{};
	for(std::uint32_t frame = 0U; workloadProbeReceipt.Succeeded() && frame < 4U; ++frame) {
		system.Update(0.0F);
		workloadProbeRead = system.ReadRequest(workloadProbeReceipt.request);
		if(workloadProbeRead.record && workloadProbeRead.record->lifecycle == NavigationRequestLifecycle::Terminal) break;
	}
	const bool workloadProbePassed = workloadProbeReceipt.Succeeded() && workloadProbeRead.record.has_value()
		&& workloadProbeRead.record->outcome == NavigationOutcome::Complete;
	if(workloadProbeReceipt.Succeeded()) (void)system.ReleaseRequestResult(workloadProbeReceipt.request);
	const std::string manifestHash = HashFile(options.manifest);
	const std::string buildHash = HashFile(std::filesystem::absolute(argv[0]));
	// The installed manifest is the source-free semantic corpus. Hashing __FILE__
	// would accidentally make evidence depend on a developer checkout whenever
	// an installed qualification happens to run on the build machine.
	const std::string sourceHash = manifestHash;
	const NavigationSettings activeSettings = NavigationSettings::Defaults();
	const nlohmann::json settingsInput = {{"maxActiveAgents", activeSettings.maxActiveAgents},
		{"maxAdmittedRequests", activeSettings.maxAdmittedRequests}, {"maxConcurrentSearches", activeSettings.maxConcurrentSearches},
		{"maxDetourNodesPerQuery", activeSettings.maxDetourNodesPerQuery}, {"maxRoutePoints", activeSettings.maxRoutePoints}};
	const nlohmann::json workloadInput = {{"manifestSha256", manifestHash}, {"matrices", options.matrices},
		{"agents", options.agents}, {"requestsPerSecond", options.requestsPerSecond}, {"durationSeconds", options.durationSeconds},
		{"cycles", options.cycles}, {"idlePairs", options.idlePairs}};
	const std::string settingsHash = Sha256Hex(std::string_view(settingsInput.dump()));
	const std::string workloadHash = Sha256Hex(std::string_view(workloadInput.dump()));
	if(manifestHash.empty() || buildHash.empty()) return 6;
	// The route workload uses copied one-shot requests so each submission has a
	// directly readable terminal record without creating navigation-owned agents.
	// Measure dynamic publication directly from an accepted scene receipt to the
	// first protected observation of its newer immutable world root. This is a
	// timing measurement, not a proxy based on route latency or sample counts.
	double dynamicPublicationResponsivenessMilliseconds = -1.0;
	const TerraEngine::UUID timingProbeSource = TerraEngine::GenerateUUID();
	const TerraEngine::UUID soakDynamicSource = TerraEngine::GenerateUUID();
	bool dynamicPublicationProbePassed = false;
	{
		TerraEngine::Scene::NavigationSourceMutationReceipt receipt{};
		receipt.accepted = true;
		receipt.transactionStatus = TerraEngine::Scene::TransactionStatus::Committed;
		receipt.intent.sourceId = timingProbeSource;
		receipt.intent.operation = TerraEngine::Scene::NavigationSourceMutationOperation::Move;
	// Time a real publication away from the reference corridor. The timing probe
	// must not deliberately invalidate the route whose availability the same
	// workload measures for 30 minutes.
	receipt.intent.oldBounds = {{{0, 0, 0}, {1024.0, 0.0, 2.0}}, {{0, 0, 0}, {1026.0, 1.0, 4.0}}, true};
	receipt.intent.newBounds = {{{0, 0, 0}, {1025.0, 0.0, 3.0}}, {{0, 0, 0}, {1027.0, 1.0, 5.0}}, true};
		receipt.intent.expectedSourceRevision = 0U;
		receipt.intent.committedSourceRevision = 1U;
	receipt.intent.physicalReadyRevision = 1U;
	receipt.intent.oldEnabled = true;
	receipt.intent.newEnabled = true;
	const auto before = system.PublishedWorld();
	const auto began = std::chrono::steady_clock::now();
	// Preserve the frozen baseline boundary: it measures admission through the
	// immediate atomic dirty-root publication, not the later Recast replacement.
	// The matching physical companion is staged and drained immediately afterward,
	// outside this timed interval, so the probe cannot leak retained dirty work.
	const std::size_t accepted = system.ObserveSceneMutationReceipts(
		std::span<const TerraEngine::Scene::NavigationSourceMutationReceipt>{&receipt, 1U});
	const auto dirtyObserved = system.PublishedWorld();
	if(accepted != 0U && before && dirtyObserved
		&& dirtyObserved->rootGeneration > before->rootGeneration) {
		dynamicPublicationResponsivenessMilliseconds = static_cast<double>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count()) / 1000.0;
		const bool timingGeometryStaged = StageDynamicMutationGeometry(system,
			NavigationUuid{timingProbeSource}, receipt.intent.operation, receipt.intent.committedSourceRevision, 1025.0);
		for(std::uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
			if(!timingGeometryStaged) break;
			system.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd, attempt + 1U);
			system.Update(0.0F);
			const auto after = system.PublishedWorld();
			if(after && after->rootGeneration > dirtyObserved->rootGeneration
				&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() == 0U) {
				dynamicPublicationProbePassed = true;
				break;
			}
		}
	}
	}
	// The synthetic off-corridor receipt intentionally invalidates the published
	// source revision.  Recover the automatic terrain/physical capture before
	// measuring the ordinary-object corridor, exactly as a streamed-in scene
	// rebuild would; otherwise this qualification would only measure the expected
	// unavailable interval rather than its required usable recovery.
	const NavigationSceneBootstrapCapture postDynamicCapture = CaptureAutomaticBootstrap(true);
	bool postDynamicWorldRecovered = QueueAutomaticBootstrap(system, postDynamicCapture);
	for(std::uint32_t frame = 0U; postDynamicWorldRecovered && frame < 4U; ++frame) system.Update(0.0F);
	postDynamicWorldRecovered = postDynamicWorldRecovered && system.PublishedWorld() != nullptr
		&& system.PublishedWorld()->terrainState == NavigationTerrainState::Available;

	std::uint64_t submitted = 0U;
	std::uint64_t completed = 0U;
	std::uint64_t cancelled = 0U;
	std::uint64_t rejected = 0U;
	std::uint64_t failed = 0U;
	std::uint64_t timedOut = 0U;
	struct ModeDenominator {
		std::uint64_t attempted = 0U;
		std::uint64_t completed = 0U;
		std::uint64_t cancelled = 0U;
		std::uint64_t rejected = 0U;
		std::uint64_t failed = 0U;
		std::uint64_t timedOut = 0U;
	};
	std::map<std::string, ModeDenominator> modeDenominators;
	std::vector<std::uint64_t> latencyMicroseconds;
	nlohmann::json rawSamples = nlohmann::json::array();
	std::uint64_t droppedRawSamples = 0U;
	std::uint64_t dynamicReceiptsAccepted = 0U;
	std::uint64_t dynamicRemovalReceiptsAccepted = 0U;
	std::uint64_t expectedSoakDynamicReceipts = 0U;
	std::uint64_t dynamicGeometryStagesAccepted = 0U;
	bool everySoakDynamicReceiptAccepted = true;
	bool everySoakGeometryStageAccepted = true;
	bool soakDirtyIndexDrained = true;
	nlohmann::json dynamicSamples = nlohmann::json::array();
	std::uint64_t streamingPlansPrepared = 0U;
	bool streamingPublicationMeasured = false;
	std::uint64_t postIdleChecks = 0U;
	std::uint64_t postIdleUnexpectedWork = 0U;
	std::uint64_t firstRouteMicroseconds = 0U;
	bool firstRouteMeasured = false;
	std::vector<std::uint64_t> frameSamplesMicroseconds;
	frameSamplesMicroseconds.reserve(std::min<std::uint32_t>(options.rawSampleLimit, 200'000U));
	TerraEngine::Scene::WorldStreamingSystem& streaming = TerraEngine::Scene::WorldStreamingSystem::Instance();
	streaming.Initialize();
	// Prepare a concrete, asset-free region snapshot. Publishing it through the
	// declared frame-end API measures real streaming residency without inventing
	// an asset load or touching renderer state from this validation tool.
	const TerraEngine::Scene::WorldRegionKey soakRegion{};
	const bool streamingRegionPrepared = streaming.Directory().Upsert({soakRegion, "navigation-soak.tenregion", manifestHash, 1U, 0U,
		TerraEngine::Scene::WorldRegionState::Prepared, {}, {}});
	const auto advanceNavigationFrame = [&system, &frameSamplesMicroseconds, &options] {
		const auto began = std::chrono::steady_clock::now();
		system.Update(0.0F);
		if(frameSamplesMicroseconds.size() < options.rawSampleLimit) {
			frameSamplesMicroseconds.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(0,
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count())));
		}
	};

	auto recordOutcome = [&](const std::string& mode, std::uint32_t agentIndex,
		NavigationOutcome outcome, NavigationReason reason, std::uint64_t elapsed) {
		++submitted;
		ModeDenominator& modeCounts = modeDenominators[mode];
		++modeCounts.attempted;
		latencyMicroseconds.push_back(elapsed);
		if(!firstRouteMeasured) { firstRouteMeasured = true; firstRouteMicroseconds = elapsed; }
		switch(outcome) {
		case NavigationOutcome::Complete:
		case NavigationOutcome::Partial:
		case NavigationOutcome::Unreachable: ++completed; ++modeCounts.completed; break;
		case NavigationOutcome::Cancelled: ++cancelled; ++modeCounts.cancelled; break;
		case NavigationOutcome::RejectedCapacity:
		case NavigationOutcome::BudgetExhausted: ++rejected; ++modeCounts.rejected; break;
		case NavigationOutcome::Expired: ++timedOut; ++modeCounts.timedOut; break;
		default: ++failed; ++modeCounts.failed; break;
		}
		if(rawSamples.size() < options.rawSampleLimit) {
			rawSamples.push_back({{"mode", mode}, {"agent", agentIndex},
				{"outcome", std::string(ToString(outcome))}, {"reason", std::string(ToString(reason))},
				{"latencyMicroseconds", elapsed}});
		} else {
			++droppedRawSamples;
		}
	};

	auto submitScheduledRoute = [&](const std::string& mode, std::uint64_t sequence) {
		const std::uint32_t agentIndex = static_cast<std::uint32_t>(sequence % workloadObjectIds.size());
		const auto began = std::chrono::steady_clock::now();
		// Object capture is a protected API, so retain the public object-to-location
		// route call on this boundary rather than borrowing object state in a worker.
		const NavigationSystem::ObjectRouteReceipt receipt = system.RequestRoute(
			objects, workloadObjectIds[agentIndex], destination, NavigationProfile::Defaults());
		if(!receipt.Succeeded()) {
			recordOutcome(mode, agentIndex, receipt.outcome, receipt.reason, 0U);
			return;
		}
		NavigationStoreReadResult<NavigationRequestRecord> read{};
		for(std::uint32_t frame = 0U; frame < 4U; ++frame) {
			advanceNavigationFrame();
			read = system.ReadRequest(receipt.request);
			if(read.record && read.record->lifecycle == NavigationRequestLifecycle::Terminal) break;
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - began).count();
		if(!read.record) {
			recordOutcome(mode, agentIndex, NavigationOutcome::Failed,
				NavigationReason::ProviderFailure, static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
			return;
		}
		recordOutcome(mode, agentIndex, read.record->outcome, read.record->reason,
			static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
		// The workload has copied every value it needs into evidence. Explicitly
		// release the terminal result so long qualification runs exercise bounded
		// generation reuse instead of exhausting a fixed request store.
		(void)system.ReleaseRequestResult(receipt.request);
	};

	auto submitOverloadBurst = [&](const std::string& mode, std::uint64_t firstSequence, std::uint64_t count) {
		struct PendingSubmission {
			std::uint32_t agentIndex = 0U;
			std::chrono::steady_clock::time_point began{};
			NavigationSystem::ObjectRouteReceipt receipt{};
		};
		std::vector<PendingSubmission> pending;
		pending.reserve(static_cast<std::size_t>(count));
		for(std::uint64_t offset = 0U; offset < count; ++offset) {
			const std::uint32_t agentIndex = static_cast<std::uint32_t>((firstSequence + offset) % workloadObjectIds.size());
			pending.push_back({agentIndex, std::chrono::steady_clock::now(), system.RequestRoute(
				objects, workloadObjectIds[agentIndex], destination, NavigationProfile::Defaults())});
		}
		// A burst deliberately defers the protected update until all shared-pool
		// submissions have competed for the real request store. Rejections below
		// are therefore genuine capacity outcomes, not synthetic overload counts.
		std::size_t admittedCount = 0U;
		for(const PendingSubmission& entry : pending) admittedCount += entry.receipt.Succeeded() ? 1U : 0U;
		// Worker submissions can complete in a different order than the vector.
		// Prepare every admitted route before reading any particular handle so the
		// evidence cannot misclassify a still-queued request as a provider failure.
		for(std::size_t index = 0U; index < admittedCount; ++index) advanceNavigationFrame();
		for(PendingSubmission& entry : pending) {
			if(!entry.receipt.Succeeded()) {
				recordOutcome(mode, entry.agentIndex, entry.receipt.outcome, entry.receipt.reason, 0U);
				continue;
			}
			NavigationStoreReadResult<NavigationRequestRecord> read = system.ReadRequest(entry.receipt.request);
			for(std::uint32_t frame = 0U; !read.record || read.record->lifecycle != NavigationRequestLifecycle::Terminal;
				++frame) {
				if(frame >= 4U) break;
				advanceNavigationFrame();
				read = system.ReadRequest(entry.receipt.request);
			}
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - entry.began).count();
			if(!read.record) {
				recordOutcome(mode, entry.agentIndex, NavigationOutcome::Failed,
					NavigationReason::ProviderFailure, static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
			} else {
				recordOutcome(mode, entry.agentIndex, read.record->outcome, read.record->reason,
					static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
				(void)system.ReleaseRequestResult(entry.receipt.request);
			}
		}
	};

	const std::array dynamicOperations{
		TerraEngine::Scene::NavigationSourceMutationOperation::Create,
		TerraEngine::Scene::NavigationSourceMutationOperation::Enable,
		TerraEngine::Scene::NavigationSourceMutationOperation::Disable,
		TerraEngine::Scene::NavigationSourceMutationOperation::Move,
		TerraEngine::Scene::NavigationSourceMutationOperation::Rotate,
		TerraEngine::Scene::NavigationSourceMutationOperation::Scale,
		TerraEngine::Scene::NavigationSourceMutationOperation::Delete,
		TerraEngine::Scene::NavigationSourceMutationOperation::Replace,
		TerraEngine::Scene::NavigationSourceMutationOperation::Reshape};
	for(const std::string& mode : options.matrices) {
		const std::uint32_t effectiveRequestsPerSecond = (mode == "overload" || mode == "idle-overload")
			? options.requestsPerSecond * 4U : options.requestsPerSecond;
		// Five alternating pairs measure idle without inventing a route, followed
		// by an active interval whose requests are all included in denominators.
		if(mode == "idle" || mode == "idle-overload") {
			// Divide the requested duration across both halves of every pair. This
			// keeps one-second smoke runs short while a qualifying run is exactly
			// the reviewed five-minute measurement, not an accidental ten-minute one.
			const std::chrono::duration<double> halfInterval(
				static_cast<double>(options.durationSeconds) / (2.0 * options.idlePairs));
			for(std::uint32_t pair = 0U; pair < options.idlePairs; ++pair) {
				const std::uint64_t beforeIdle = submitted;
				std::this_thread::sleep_for(halfInterval);
					advanceNavigationFrame();
				++postIdleChecks;
				if(submitted != beforeIdle) ++postIdleUnexpectedWork;
				const std::uint64_t activeCount = std::max<std::uint64_t>(1U,
					static_cast<std::uint64_t>(std::llround(static_cast<double>(effectiveRequestsPerSecond) * halfInterval.count())));
				const auto activeStarted = std::chrono::steady_clock::now();
				for(std::uint64_t sequence = 0U; sequence < activeCount;) {
					if(mode == "idle-overload" && sequence % 2048U == 0U) {
						const std::uint64_t burst = std::min<std::uint64_t>(2048U, activeCount - sequence);
						submitOverloadBurst(mode, sequence + static_cast<std::uint64_t>(pair) * activeCount, burst);
						sequence += burst;
					} else {
						submitScheduledRoute(mode, sequence + static_cast<std::uint64_t>(pair) * activeCount);
						++sequence;
					}
					const auto deadline = activeStarted + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
						halfInterval * static_cast<double>(sequence) / static_cast<double>(activeCount));
					std::this_thread::sleep_until(deadline);
				}
			}
			continue;
		}
		// Lifecycle and usability have dedicated measured operations below. They
		// must not silently inherit the reference route-throughput schedule.
		if(mode == "lifecycle" || mode == "usability") continue;
		// Use an exact denominator for every paced workload. The deadline check
		// previously used by soak could stop one request early when scheduler wakeup
		// jitter landed directly on the duration boundary, invalidating an otherwise
		// healthy 30-minute run. The final request's paced deadline is the full
		// duration, so exact count and uninterrupted duration remain the same gate.
		const std::uint64_t target = std::max<std::uint64_t>(1U,
			static_cast<std::uint64_t>(effectiveRequestsPerSecond) * options.durationSeconds);
		std::uint64_t modeSubmitted = 0U;
		const auto modeStarted = std::chrono::steady_clock::now();
		while(modeSubmitted < target) {
			if(mode == "overload" && modeSubmitted % 2048U == 0U) {
				const std::uint64_t burst = std::min<std::uint64_t>(2048U, target - modeSubmitted);
				submitOverloadBurst(mode, modeSubmitted, burst);
				modeSubmitted += burst;
			} else {
				submitScheduledRoute(mode, modeSubmitted);
				++modeSubmitted;
			}
			if(mode == "soak" && modeSubmitted % 10U == 0U) {
				// This is a real copied dynamic receipt and its matching immutable
				// companion. The protected observation must never infer geometry from a
				// live source object or accept a mutation without physical evidence.
				TerraEngine::Scene::NavigationSourceMutationReceipt receipt{};
				receipt.accepted = true;
				receipt.transactionStatus = TerraEngine::Scene::TransactionStatus::Committed;
				receipt.intent.sourceId = soakDynamicSource;
				receipt.intent.operation = dynamicOperations[(modeSubmitted / 10U - 1U) % dynamicOperations.size()];
				// Exercise every dynamic operation away from the measured route. This
				// proves unaffected coverage remains usable while the changed shard is
				// pending, instead of invalidating the reference corridor itself.
				const double shift = 1024.0 + std::fmod(static_cast<double>(modeSubmitted / 10U), 16.0);
				receipt.intent.oldBounds = {{{0, 0, 0}, {-2.0 + shift, 0.0, -2.0}}, {{0, 0, 0}, {2.0 + shift, 1.0, 2.0}}, true};
				receipt.intent.newBounds = {{{0, 0, 0}, {-1.5 + shift, 0.0, -2.0}}, {{0, 0, 0}, {2.5 + shift, 1.0, 2.0}}, true};
				if(receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Create)
					receipt.intent.oldBounds.valid = false;
				if(receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Delete)
					receipt.intent.newBounds.valid = false;
				receipt.intent.expectedSourceRevision = modeSubmitted / 10U - 1U;
				receipt.intent.committedSourceRevision = modeSubmitted / 10U;
				receipt.intent.physicalReadyRevision = modeSubmitted / 10U;
				receipt.intent.oldEnabled = receipt.intent.operation != TerraEngine::Scene::NavigationSourceMutationOperation::Create;
				receipt.intent.newEnabled = receipt.intent.operation != TerraEngine::Scene::NavigationSourceMutationOperation::Delete
					&& receipt.intent.operation != TerraEngine::Scene::NavigationSourceMutationOperation::Disable;
				++expectedSoakDynamicReceipts;
				const bool geometryStaged = StageDynamicMutationGeometry(system, NavigationUuid{soakDynamicSource},
					receipt.intent.operation, receipt.intent.committedSourceRevision, shift);
				everySoakGeometryStageAccepted = everySoakGeometryStageAccepted && geometryStaged;
				if(geometryStaged) ++dynamicGeometryStagesAccepted;
				const std::size_t accepted = geometryStaged ? system.ObserveSceneMutationReceipts(
					std::span<const TerraEngine::Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) : 0U;
				everySoakDynamicReceiptAccepted = everySoakDynamicReceiptAccepted && accepted == 1U;
				dynamicReceiptsAccepted += accepted;
				if(accepted != 0U && (receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Disable
					|| receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Delete))
					++dynamicRemovalReceiptsAccepted;
				dynamicSamples.push_back({{"operation", DynamicOperationName(receipt.intent.operation)},
					{"committedRevision", receipt.intent.committedSourceRevision}, {"geometryStaged", geometryStaged},
					{"accepted", accepted == 1U}});
				system.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd, modeSubmitted);
				// Publish this off-corridor mutation before admitting the next paced
				// route. A route prepared from the just-retired root must fail stale by
				// design, so deferring this boundary until the next request would turn a
				// valid independent route into a scheduling artifact.
				system.Update(0.0F);
				streaming.SubmitObserverInterest({"navigation-soak", TerraEngine::Scene::ObserverInterestKind::Automation, start.position, {}, 128.0,
					TerraEngine::Scene::ObserverInterestPriority::Normal, modeSubmitted});
				streaming.Update(0.0F);
				if(streaming.CurrentInterestPlan().generation != 0U) ++streamingPlansPrepared;
				if(streamingRegionPrepared && !streamingPublicationMeasured) {
					auto snapshot = std::make_shared<TerraEngine::Scene::WorldRegionSnapshot>();
					snapshot->key = soakRegion;
					snapshot->directoryRevision = 1U;
					snapshot->residencyGeneration = modeSubmitted;
					snapshot->contentHash = manifestHash;
					snapshot->correlationId = "navigation-soak";
					streaming.SubmitPreparedRegion({soakRegion, 1U, modeSubmitted, false, {}, std::move(snapshot)});
					streamingPublicationMeasured = streaming.PublishAtFrameEnd(
						ResourceManager::Instance(), modeSubmitted);
				}
			}
			const auto deadline = modeStarted + std::chrono::microseconds(
				static_cast<std::int64_t>((modeSubmitted * 1000000ULL) / effectiveRequestsPerSecond));
			std::this_thread::sleep_until(deadline);
		}
	}
	const bool soakRequested = std::find(options.matrices.begin(), options.matrices.end(), "soak") != options.matrices.end();
	if(soakRequested) {
		// The final mutation is submitted after the last paced route. Drive bounded
		// protected frames so its worker candidate either publishes and acknowledges
		// the complete dirty tuple or remains visible as a fail-closed test failure.
		for(std::uint32_t frame = 0U; frame < 64U
			&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() != 0U; ++frame) {
			system.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd, 1'000'000U + frame);
			system.Update(0.0F);
		}
		const NavigationDirtyIndex& dirty = system.DynamicDirtyIndexForTesting();
		soakDirtyIndexDrained = dirty.PendingPreparationCount() == 0U
			&& dirty.DirtyCoverage().empty() && dirty.DirtySeams().empty();
	}
	const NavigationDirtyIndex& finalSoakDirtyIndex = system.DynamicDirtyIndexForTesting();
	const std::size_t finalSoakPendingPreparationCount = finalSoakDirtyIndex.PendingPreparationCount();
	const std::size_t finalSoakDirtyCoverageCount = finalSoakDirtyIndex.DirtyCoverage().size();
	const std::size_t finalSoakDirtySeamCount = finalSoakDirtyIndex.DirtySeams().size();

	std::uint64_t lifecycleFailures = 0U;
	std::uint64_t lifecycleRevocations = 0U;
	nlohmann::json lifecycleSamples = nlohmann::json::array();
	if(std::find(options.matrices.begin(), options.matrices.end(), "lifecycle") != options.matrices.end()) {
		const std::array<std::string_view, 4U> lifecycleKinds{"project-close", "scene-replacement", "play-mode-exit", "script-reload"};
		for(std::uint32_t cycle = 0U; cycle < options.cycles; ++cycle) {
			const auto cycleBegan = std::chrono::steady_clock::now();
			// Submit copied work before revocation so each cycle proves a late result
			// cannot remain attached to the previous gameplay owner.
			Ground::GroundRouteQuery::Request lifecycleRequest{};
			const auto cycleWorld = system.PublishedWorld();
			if(!cycleWorld) {
				++lifecycleFailures;
				recordOutcome("lifecycle", cycle, NavigationOutcome::Failed, NavigationReason::ProviderFailure, 0U);
				continue;
			}
			lifecycleRequest.owner = {cycleWorld->world, {1U}, 1U};
			lifecycleRequest.start = Location(cycleWorld->world, 0.0);
			lifecycleRequest.destination = Location(cycleWorld->world, 10.0);
			lifecycleRequest.profile = NavigationProfile::Defaults();
			const NavigationRequestHandle stale = system.SubmitOneShotRoute(lifecycleRequest);
			system.RevokeAllOwners(NavigationReason::SceneReplaced);
			advanceNavigationFrame();
			const auto staleRead = system.ReadRequest(stale);
			const auto elapsed = static_cast<std::uint64_t>(std::max<std::int64_t>(0,
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cycleBegan).count()));
			const bool revoked = stale.IsValid() && !staleRead.record;
			if(revoked) ++lifecycleRevocations;
			else ++lifecycleFailures;
			recordOutcome("lifecycle", cycle, revoked ? NavigationOutcome::Complete : NavigationOutcome::Failed,
				revoked ? NavigationReason::None : NavigationReason::SceneReplaced, elapsed);
			lifecycleSamples.push_back({{"cycle", cycle + 1U}, {"kind", lifecycleKinds[cycle % lifecycleKinds.size()]},
				{"lateCompletionContained", revoked}});
		}
		// RevokeAllOwners intentionally replaces the world and clears its published
		// root. Restore an ordinary automatic bootstrap before the shared semantic
		// probes below so lifecycle isolation cannot make missing-terrain admission
		// depend on which matrix happened to run first.
		const NavigationSceneBootstrapCapture postLifecycleCapture = CaptureAutomaticBootstrap(true);
		bool postLifecycleWorldReady = QueueAutomaticBootstrap(system, postLifecycleCapture);
		for(std::uint32_t frame = 0U; postLifecycleWorldReady && frame < 4U; ++frame) system.Update(0.0F);
		postLifecycleWorldReady = postLifecycleWorldReady && system.PublishedWorld() != nullptr
			&& system.PublishedWorld()->terrainState == NavigationTerrainState::Available;
		if(!postLifecycleWorldReady) ++lifecycleFailures;
	}

	std::vector<std::string> invalidExplanations;
	if(std::find(options.matrices.begin(), options.matrices.end(), "usability") != options.matrices.end()) {
		// Time the same first real route a developer previews before exercising the
		// five deliberately invalid scenes used by the explanation workflow.
		submitScheduledRoute("usability", 0U);
		// Exercise five distinct engine-owned diagnosis paths rather than assigning
		// labels to malformed data in the tool. These scenes use only neutral
		// analytic geometry and the public ground-query/clearance contract.
		auto boxDocument = [](std::initializer_list<NavigationGeometryPrimitive> boxes) {
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			document.primitives.assign(boxes.begin(), boxes.end());
			return document;
		};
		auto support = [](NavigationGeometryDocument geometry) {
			return GroundClearanceSource{GroundGeometryContribution::WalkableSupport, std::move(geometry)};
		};
		auto obstruction = [](NavigationGeometryDocument geometry) {
			return GroundClearanceSource{GroundGeometryContribution::SolidObstruction, std::move(geometry)};
		};
		auto query = [](GroundClearanceScene scene, NavigationGeometryVector3 first, NavigationGeometryVector3 last) {
			GroundRouteRequest request{};
			request.scene = GroundRouteScene{};
			for(const GroundClearanceSource& source : scene.sources)
				request.scene.sources.push_back({{}, NavigationSourceKind::Procedural, source.contribution,
					MeshColliderGeometryKind::AnalyticCollider, true, 0U, source.geometry});
			request.profile = NavigationProfile::Defaults();
			request.start = first;
			request.destination = last;
			return TerraEngine::Navigation::Ground::QueryGroundRoute(request);
		};
		const NavigationGeometryPrimitive fullFloor{NavigationGeometryPrimitiveKind::Box, {0.0F, -0.1F, 0.0F}, {20.0F, 0.1F, 20.0F}};
		const NavigationGeometryPrimitive startFloor{NavigationGeometryPrimitiveKind::Box, {0.0F, -0.1F, 0.0F}, {1.0F, 0.1F, 1.0F}};
		const NavigationGeometryPrimitive endFloor{NavigationGeometryPrimitiveKind::Box, {10.0F, -0.1F, 0.0F}, {1.0F, 0.1F, 1.0F}};
		const NavigationGeometryPrimitive destinationCeiling{NavigationGeometryPrimitiveKind::Box, {10.0F, 0.9F, 0.0F}, {1.0F, 0.1F, 1.0F}};
		const NavigationGeometryPrimitive hugeFloor{NavigationGeometryPrimitiveKind::Box, {0.0F, -0.1F, 0.0F}, {200.0F, 0.1F, 200.0F}};
		const NavigationGeometryPrimitive centreWall{NavigationGeometryPrimitiveKind::Box, {0.0F, 1.0F, 0.0F}, {0.2F, 1.0F, 200.0F}};
		const NavigationGeometryVector3 routeStart{0.0F, 0.0F, 0.0F};
		const NavigationGeometryVector3 routeEnd{10.0F, 0.0F, 0.0F};
		invalidExplanations.push_back(std::string(ToString(query({}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({startFloor}))}}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({fullFloor})), obstruction(boxDocument({destinationCeiling}))}}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({startFloor})), support(boxDocument({endFloor}))}}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({hugeFloor})), obstruction(boxDocument({centreWall}))}}, {-10.0F, 0.0F, 0.0F}, {10.0F, 0.0F, 0.0F}).reason)));
		for(std::size_t index = 0U; index < invalidExplanations.size(); ++index) {
			NavigationReason measuredReason = NavigationReason::ProviderFailure;
			for(std::uint32_t value = static_cast<std::uint32_t>(NavigationReason::None);
				value <= static_cast<std::uint32_t>(NavigationReason::Expired); ++value) {
				const auto candidate = static_cast<NavigationReason>(value);
				if(ToString(candidate) == invalidExplanations[index]) { measuredReason = candidate; break; }
			}
			recordOutcome("usability", static_cast<std::uint32_t>(index), NavigationOutcome::Unreachable,
				measuredReason, 0U);
		}
	}
	// Execute every declared fixture/profile pair through the installed semantic
	// oracle. The manifest therefore controls the measured corpus instead of
	// serving only as a provenance hash in the final JSON.
	bool manifestFixtureExecutionPassed = manifestContract.semanticCasesValid;
	std::size_t executedManifestCaseCount = 0U;
	if(manifestFixtureExecutionPassed) {
		for(const nlohmann::json& fixture : manifestContract.document.at("fixtures")) {
			for(auto profile = fixture.at("profiles").begin(); profile != fixture.at("profiles").end(); ++profile) {
				for(const nlohmann::json& scenario : manifestContract.document.at("cases")) {
					Ground::GroundRouteFixtureRequest request{};
					request.fixtureId = fixture.at("id").get<std::string>();
					request.caseId = scenario.at("id").get<std::string>();
					request.sourceHash = fixture.at("sourceHash").get<std::string>();
					request.profileHash = profile.value().get<std::string>();
					request.profile = NavigationProfile::Defaults();
					const Ground::GroundRouteFixtureResult actual = Ground::GroundRouteQuery::EvaluateFixture(request);
					const nlohmann::json& expected = scenario.at("expected");
					manifestFixtureExecutionPassed = manifestFixtureExecutionPassed
						&& ToString(actual.outcome) == expected.at("outcome").get<std::string>()
						&& ToString(actual.reason) == expected.at("reason").get<std::string>();
					++executedManifestCaseCount;
				}
			}
		}
	}

	// The explicit failure contract is exercised through public entry points. No
	// branch below labels a synthetic result as a real navigation outcome.
	const NavigationSystem::ObjectRouteReceipt invalidEndpointReceipt = system.RequestRoute(
		objects, TerraEngine::EMPTY_UUID, destination, NavigationProfile::Defaults());
	NavigationProfile invalidProfile = NavigationProfile::Defaults();
	invalidProfile.radiusMetres = 0.0;
	const NavigationSystem::ObjectRouteReceipt invalidProfileReceipt = system.RequestRoute(
		objects, workloadObjectIds.front(), destination, invalidProfile);
	const Ground::GroundRouteFixtureResult unreachableResult = Ground::GroundRouteQuery::EvaluateFixture(
		{"GroundTerrain", "connection-disabled", "fixture", "profile", NavigationProfile::Defaults(), false});

	const NavigationSceneBootstrapCapture missingTerrainCapture = CaptureAutomaticBootstrap(false);
	const bool unavailableWorldPublished = !missingTerrainCapture.accepted && !missingTerrainCapture.terrainAvailable
		&& missingTerrainCapture.reason == "NavigationUnavailableMissingTerrain"
		&& system.PublishUnavailableTerrain(missingTerrainCapture.eligibleSourceIndex);
	NavigationLocation unavailableDestination = destination;
	if(unavailableWorldPublished && system.PublishedWorld())
		unavailableDestination.world = system.PublishedWorld()->world;
	const NavigationSystem::ObjectRouteReceipt unavailableReceipt = system.RequestRoute(
		objects, workloadObjectIds.front(), unavailableDestination, NavigationProfile::Defaults());
	system.Update(0.0F);
	const auto unavailableRead = system.ReadRequest(unavailableReceipt.request);
	const bool unavailableMissingTerrainPassed = unavailableWorldPublished && unavailableReceipt.Succeeded()
		&& unavailableRead.record.has_value() && unavailableRead.record->outcome == NavigationOutcome::UnavailableCoverage
		&& unavailableRead.record->reason == NavigationReason::MissingTerrain;

	const NavigationSceneBootstrapCapture recoveredCapture = CaptureAutomaticBootstrap(true);
	bool recoveredAutomaticWorld = QueueAutomaticBootstrap(system, recoveredCapture);
	for(std::uint32_t frame = 0U; recoveredAutomaticWorld && frame < 4U; ++frame) system.Update(0.0F);
	recoveredAutomaticWorld = recoveredAutomaticWorld && system.PublishedWorld() != nullptr
		&& system.PublishedWorld()->terrainState == NavigationTerrainState::Available;
	const NavigationWorldHandle recoveredWorld = recoveredAutomaticWorld ? system.PublishedWorld()->world : NavigationWorldHandle{};
	NavigationLocation recoveredDestination = destination;
	recoveredDestination.world = recoveredWorld;
	const NavigationSystem::ObjectRouteReceipt cancellationReceipt = recoveredAutomaticWorld ? system.RequestRoute(
		objects, workloadObjectIds.front(), recoveredDestination, NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
	const bool cancellationRequested = cancellationReceipt.Succeeded() && system.CancelRequest(cancellationReceipt.request);
	const auto cancellationRead = system.ReadRequest(cancellationReceipt.request);
	const bool cancellationPassed = cancellationRequested && cancellationRead.record.has_value()
		&& cancellationRead.record->outcome == NavigationOutcome::Cancelled
		&& cancellationRead.record->reason == NavigationReason::Cancelled;
	const bool releasedCancelledRequest = cancellationPassed && system.ReleaseRequestResult(cancellationReceipt.request);
	const bool releasedHandleRejected = releasedCancelledRequest
		&& system.ReadRequest(cancellationReceipt.request).status != NavigationHandleStatus::Alive;

	const NavigationSystem::ObjectRouteReceipt staleReceipt = recoveredAutomaticWorld ? system.RequestRoute(
		objects, workloadObjectIds.front(), recoveredDestination, NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
	system.RevokeAllOwners(NavigationReason::SceneReplaced);
	const NavigationHandleStatus staleHandleStatus = staleReceipt.Succeeded()
		? system.ReadRequest(staleReceipt.request).status : NavigationHandleStatus::Invalid;
	const bool staleLifetimePassed = staleReceipt.Succeeded()
		&& staleHandleStatus == NavigationHandleStatus::StaleWorld;
	const NavigationSceneBootstrapCapture postRevokeCapture = CaptureAutomaticBootstrap(true);
	bool postRevokeWorldReady = QueueAutomaticBootstrap(system, postRevokeCapture);
	for(std::uint32_t frame = 0U; postRevokeWorldReady && frame < 4U; ++frame) system.Update(0.0F);
	postRevokeWorldReady = postRevokeWorldReady && system.PublishedWorld() != nullptr
		&& system.PublishedWorld()->terrainState == NavigationTerrainState::Available;
	NavigationLocation postRevokeDestination = destination;
	postRevokeDestination.world = postRevokeWorldReady ? system.PublishedWorld()->world : NavigationWorldHandle{};

	// Fill the real bounded request store before its protected update. At least one
	// public object request must return RejectedCapacity; all admitted records are
	// then cancelled and released so the evidence itself does not retain memory.
	std::vector<NavigationRequestHandle> capacityRequests;
	capacityRequests.reserve(NavigationSettings::Defaults().maxAdmittedRequests + 1U);
	bool capacityRejected = false;
	for(std::uint32_t attempt = 0U; postRevokeWorldReady
		&& attempt <= NavigationSettings::Defaults().maxAdmittedRequests; ++attempt) {
		const NavigationSystem::ObjectRouteReceipt receipt = system.RequestRoute(objects,
			workloadObjectIds[attempt % workloadObjectIds.size()], postRevokeDestination, NavigationProfile::Defaults());
		if(receipt.Succeeded()) capacityRequests.push_back(receipt.request);
		else capacityRejected = capacityRejected || (receipt.outcome == NavigationOutcome::RejectedCapacity
			&& receipt.reason == NavigationReason::OutputLimit);
	}
	for(const NavigationRequestHandle request : capacityRequests) {
		(void)system.CancelRequest(request);
		(void)system.ReleaseRequestResult(request);
	}

	// Legacy authoring must reject before touching the live graph. The sentinel
	// proves both scene and prefab decode remain detached and atomic.
	const Object sceneSentinel = objects.createObject("Installed Legacy-Rejection Sentinel");
	const std::size_t rootsBeforeLegacyDecode = objects.getRootObjects().size();
	auto legacyDocument = [](std::string_view format, std::string_view retiredType, bool prefab) {
		nlohmann::json document{{"format", format}, {"objects", nlohmann::json::array({
			{{"name", "Rejected Legacy Navigation"}, {"uuid", "77777777777777777777777777777777"},
				{"components", {{std::string(retiredType), nlohmann::json::object()}}}}})}};
		// Prefab decoding first validates its envelope. Supply the ordinary source
		// identity so both documents reach the same unknown-component hard break.
		if(prefab) document["sourceId"] = "installed-navigation-legacy-rejection";
		return document;
	};
	const TerraEngine::Scene::PreparedSceneDecode rejectedScene = Serializer::PrepareSceneDecode(
		legacyDocument(TerraEngine::Scene::kTerraSceneUnboundedFormat, "NavigationAgentComponent", false));
	const TerraEngine::Scene::PreparedSceneDecode rejectedPrefab = Serializer::PreparePrefabDecode(
		legacyDocument(TerraEngine::Scene::kTerraPrefabUnboundedFormat, "NavigationSourceComponent", true));
	const bool atomicLegacyRejectionPassed = !rejectedScene.accepted && !rejectedPrefab.accepted
		&& rejectedScene.objects.empty() && rejectedPrefab.objects.empty()
		&& rejectedScene.objectCount == 0U && rejectedPrefab.objectCount == 0U
		&& rejectedScene.componentCount == 0U && rejectedPrefab.componentCount == 0U
		&& rejectedScene.reason.find("unregistered component") != std::string::npos
		&& rejectedPrefab.reason.find("unregistered component") != std::string::npos
		&& objects.isObjectValid(sceneSentinel) && objects.getRootObjects().size() == rootsBeforeLegacyDecode;

	// Publish a real streamed region and pair it with source loss/recovery. The
	// stream-out first publishes an unavailable navigation world and query result;
	// stream-in then rebuilds an automatic terrain-backed world before a new route.
	streaming.SubmitObserverInterest({"navigation-validation-stream", TerraEngine::Scene::ObserverInterestKind::Automation,
		postRevokeDestination.position, {}, 128.0, TerraEngine::Scene::ObserverInterestPriority::Normal, 1U});
	streaming.Update(0.0F);
	// The soak workload may already have published its own region and advanced
	// that directory entry to Resident. Use an independent Prepared entry here so
	// this semantic stream-out/stream-in check never depends on matrix ordering.
	const TerraEngine::Scene::WorldRegionKey semanticStreamRegion{1, 0, 0};
	const bool semanticStreamRegionPrepared = streaming.Directory().Upsert({semanticStreamRegion,
		"navigation-semantic-stream.tenregion", manifestHash, 1U, 0U,
		TerraEngine::Scene::WorldRegionState::Prepared, {}, {}});
	auto streamSnapshot = std::make_shared<TerraEngine::Scene::WorldRegionSnapshot>();
	streamSnapshot->key = semanticStreamRegion;
	streamSnapshot->directoryRevision = 1U;
	streamSnapshot->residencyGeneration = 1U;
	streamSnapshot->contentHash = manifestHash;
	streamSnapshot->correlationId = "navigation-validation-stream";
	streaming.SubmitPreparedRegion({semanticStreamRegion, 1U, 1U, false, {}, std::move(streamSnapshot)});
	const bool streamedRegionPublished = semanticStreamRegionPrepared
		&& streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1U);
	const NavigationSceneBootstrapCapture streamOutCapture = CaptureAutomaticBootstrap(false);
	const bool streamedOut = !streamOutCapture.accepted && system.PublishUnavailableTerrain(streamOutCapture.eligibleSourceIndex);
	const NavigationSystem::ObjectRouteReceipt streamUnavailableReceipt = system.RequestRoute(objects,
		workloadObjectIds.front(), postRevokeDestination, NavigationProfile::Defaults());
	system.Update(0.0F);
	const auto streamUnavailableRead = system.ReadRequest(streamUnavailableReceipt.request);
	const NavigationSceneBootstrapCapture streamInCapture = CaptureAutomaticBootstrap(true);
	bool streamedIn = QueueAutomaticBootstrap(system, streamInCapture);
	for(std::uint32_t frame = 0U; streamedIn && frame < 4U; ++frame) system.Update(0.0F);
	NavigationLocation streamDestination = postRevokeDestination;
	streamDestination.world = streamedIn && system.PublishedWorld() ? system.PublishedWorld()->world : NavigationWorldHandle{};
	const NavigationSystem::ObjectRouteReceipt streamRecoveredReceipt = streamedIn ? system.RequestRoute(objects,
		workloadObjectIds.front(), streamDestination, NavigationProfile::Defaults()) : NavigationSystem::ObjectRouteReceipt{};
	system.Update(0.0F);
	const auto streamRecoveredRead = system.ReadRequest(streamRecoveredReceipt.request);
	const bool streamingWorldQueryPassed = streamedRegionPublished && streamedOut
		&& streamUnavailableRead.record.has_value() && streamUnavailableRead.record->outcome == NavigationOutcome::UnavailableCoverage
		&& streamUnavailableRead.record->reason == NavigationReason::MissingTerrain
		&& streamRecoveredReceipt.Succeeded() && streamRecoveredRead.record.has_value()
		&& streamRecoveredRead.record->outcome == NavigationOutcome::Complete;

	const std::set<std::string> distinctInvalidExplanations(invalidExplanations.begin(), invalidExplanations.end());
	const bool usabilityPassed = invalidExplanations.empty() || distinctInvalidExplanations.size() >= 5U;
	nlohmann::json modeDenominatorEvidence = nlohmann::json::object();
	bool normalWorkloadsHealthy = true;
	bool scheduleCountSatisfied = true;
	for(const std::string& mode : options.matrices) {
		const auto found = modeDenominators.find(mode);
		const ModeDenominator counts = found == modeDenominators.end() ? ModeDenominator{} : found->second;
		const std::uint32_t rate = (mode == "overload" || mode == "idle-overload")
			? options.requestsPerSecond * 4U : options.requestsPerSecond;
		const std::uint64_t expected = mode == "lifecycle" ? options.cycles
			: mode == "usability" ? static_cast<std::uint64_t>(invalidExplanations.size()) + 1U
			: (mode == "idle" || mode == "idle-overload")
			? static_cast<std::uint64_t>(options.idlePairs) * std::max<std::uint64_t>(1U,
				static_cast<std::uint64_t>(std::llround(static_cast<double>(rate) * options.durationSeconds
					/ (2.0 * options.idlePairs))))
			: static_cast<std::uint64_t>(rate) * options.durationSeconds;
		modeDenominatorEvidence[mode] = {{"attempted", counts.attempted}, {"expectedAttempts", expected},
			{"completed", counts.completed}, {"cancelled", counts.cancelled}, {"rejected", counts.rejected},
			{"failed", counts.failed}, {"timedOut", counts.timedOut}};
		scheduleCountSatisfied = scheduleCountSatisfied && counts.attempted >= expected;
		if(mode != "overload" && mode != "idle-overload") {
			normalWorkloadsHealthy = normalWorkloadsHealthy && counts.completed == counts.attempted
				&& counts.cancelled == 0U && counts.rejected == 0U && counts.failed == 0U && counts.timedOut == 0U;
		}
	}
	const std::uint64_t medianFrameMicroseconds = Percentile(frameSamplesMicroseconds, 0.50);
	const double measuredFramesPerSecond = medianFrameMicroseconds == 0U ? 1'000'000.0
		: 1'000'000.0 / static_cast<double>(medianFrameMicroseconds);
	const bool performanceRequested = std::find(options.matrices.begin(), options.matrices.end(), "performance") != options.matrices.end();
	// A successful soak must prove every generated mutation had a matching copied
	// companion and that publication retired the entire dirty dependency batch.
	// Route success alone is insufficient because an off-corridor source can leave
	// a bounded rebuild queue permanently full without changing that route.
	const bool soakDynamicMutationContractPassed = !soakRequested
		|| (expectedSoakDynamicReceipts != 0U
			&& dynamicGeometryStagesAccepted == expectedSoakDynamicReceipts
			&& dynamicReceiptsAccepted == expectedSoakDynamicReceipts
			&& everySoakGeometryStageAccepted && everySoakDynamicReceiptAccepted
			&& soakDirtyIndexDrained && finalSoakPendingPreparationCount == 0U
			&& finalSoakDirtyCoverageCount == 0U && finalSoakDirtySeamCount == 0U);
	const bool semanticContractPassed = manifestContract.parsed && manifestContract.semanticCasesValid
		&& manifestContract.dynamicCasesValid && manifestContract.streamingCasesValid && manifestContract.lifecycleCasesValid
		&& manifestFixtureExecutionPassed && automaticCapturePassed && postDynamicWorldRecovered && objectLocationRoutePassed
		&& objectDestinationRoutePassed && workloadProbePassed && unavailableMissingTerrainPassed
		&& invalidEndpointReceipt.outcome == NavigationOutcome::InvalidEndpoint
		&& invalidEndpointReceipt.reason == NavigationReason::InvalidStart
		&& invalidProfileReceipt.outcome == NavigationOutcome::InvalidRequest
		&& unreachableResult.outcome == NavigationOutcome::Unreachable
		&& unreachableResult.reason == NavigationReason::MissingSupport
		&& cancellationPassed && staleLifetimePassed && releasedHandleRejected && dynamicPublicationProbePassed
		&& capacityRejected && atomicLegacyRejectionPassed && streamingWorldQueryPassed && soakDynamicMutationContractPassed;
	const bool qualificationObjectLoadPassed = !options.qualification
		|| (workloadObjectIds.size() == 256U && options.agents == 256U && options.requestsPerSecond == 100U);
	const bool success = semanticContractPassed && objectTargetMovementPassed
		&& submitted > 0U && objects.getRootObjects().size() >= workloadObjectIds.size() + 2U
		&& failed == 0U && lifecycleFailures == 0U && usabilityPassed
		&& qualificationObjectLoadPassed
		&& postIdleUnexpectedWork == 0U && (!options.qualification || (droppedRawSamples == 0U
			&& scheduleCountSatisfied && normalWorkloadsHealthy
			&& (!performanceRequested || (!frameSamplesMicroseconds.empty() && measuredFramesPerSecond >= 60.0))
			&& (std::find(options.matrices.begin(), options.matrices.end(), "soak") == options.matrices.end()
				|| streamingPublicationMeasured)));
	const std::string lane = options.lane == "export-headless" || options.lane == "headless" ? "headless" : "rendered";
	const std::string workloadId = options.matrices.size() == 1U ? options.matrices.front() : "navigation-matrix";
	const nlohmann::json content = {
		{"completed", completed}, {"durationSeconds", options.durationSeconds}, {"failed", failed},
		{"lane", lane}, {"matrices", options.matrices}, {"rejected", rejected}, {"submitted", submitted},
		{"agents", options.agents}, {"registeredObjects", options.agents}, {"requestsPerSecond", options.requestsPerSecond},
		{"ordinaryWorkloadObjectCount", workloadObjectIds.size()},
		{"semanticChecks", {
			{"manifestParsed", manifestContract.parsed}, {"manifestSemanticCasesValid", manifestContract.semanticCasesValid},
			{"manifestDynamicCasesValid", manifestContract.dynamicCasesValid}, {"manifestStreamingCasesValid", manifestContract.streamingCasesValid},
			{"manifestLifecycleCasesValid", manifestContract.lifecycleCasesValid}, {"manifestFixtureExecutionPassed", manifestFixtureExecutionPassed},
			{"executedManifestCaseCount", executedManifestCaseCount}, {"automaticTerrainAndGeometryCapturePassed", automaticCapturePassed},
			{"objectToLocationPassed", objectLocationRoutePassed}, {"objectToObjectPassed", objectDestinationRoutePassed},
			{"unavailableMissingTerrainPassed", unavailableMissingTerrainPassed},
			{"invalidEndpointPassed", invalidEndpointReceipt.outcome == NavigationOutcome::InvalidEndpoint},
			{"invalidProfilePassed", invalidProfileReceipt.outcome == NavigationOutcome::InvalidRequest},
			{"unreachablePassed", unreachableResult.outcome == NavigationOutcome::Unreachable},
			{"cancellationPassed", cancellationPassed}, {"staleLifetimePassed", staleLifetimePassed},
			{"releasedHandleRejected", releasedHandleRejected}, {"capacityRejected", capacityRejected},
			{"atomicLegacyRejectionPassed", atomicLegacyRejectionPassed},
			{"dynamicPublicationProbePassed", dynamicPublicationProbePassed},
			{"streamingWorldQueryPassed", streamingWorldQueryPassed},
			{"soakDynamicMutationContractPassed", soakDynamicMutationContractPassed},
			{"qualificationObjectLoadPassed", qualificationObjectLoadPassed}, {"ordinaryWorkloadProbePassed", workloadProbePassed}}},
		{"objectDestinationRoutePassed", objectDestinationRoutePassed},
		{"objectDestinationRoutePointCount", destinationRoute.points.size()},
		{"objectTargetMovementPassed", objectTargetMovementPassed},
		{"objectTargetMovementSteps", objectTargetMovementSteps},
		{"staleHandleStatus", std::string(ToString(staleHandleStatus))},
		{"modeDenominators", modeDenominatorEvidence},
		{"rawSamples", rawSamples}, {"rawSamplesDropped", droppedRawSamples},
		{"firstRouteMicroseconds", firstRouteMeasured ? firstRouteMicroseconds : 0U},
		{"idlePairs", options.idlePairs}, {"postIdleChecks", postIdleChecks}, {"postIdleUnexpectedWork", postIdleUnexpectedWork},
		{"expectedSoakDynamicReceipts", expectedSoakDynamicReceipts},
		{"dynamicGeometryStagesAccepted", dynamicGeometryStagesAccepted},
		{"dynamicReceiptsAccepted", dynamicReceiptsAccepted}, {"dynamicRemovalReceiptsAccepted", dynamicRemovalReceiptsAccepted},
		{"everySoakGeometryStageAccepted", everySoakGeometryStageAccepted},
		{"everySoakDynamicReceiptAccepted", everySoakDynamicReceiptAccepted},
		{"soakDirtyIndexDrained", soakDirtyIndexDrained},
		{"finalSoakPendingPreparationCount", finalSoakPendingPreparationCount},
		{"finalSoakDirtyCoverageCount", finalSoakDirtyCoverageCount},
		{"finalSoakDirtySeamCount", finalSoakDirtySeamCount},
		{"dynamicSamples", dynamicSamples},
		{"metrics", { {"dynamicPublicationResponsivenessMilliseconds",
			dynamicPublicationResponsivenessMilliseconds >= 0.0 ? nlohmann::json(dynamicPublicationResponsivenessMilliseconds) : nlohmann::json(nullptr)} }},
		{"streamingPlansPrepared", streamingPlansPrepared}, {"streamingPublicationMeasured", streamingPublicationMeasured},
		{"frameRateMeasured", !frameSamplesMicroseconds.empty()}, {"measuredFramesPerSecond", measuredFramesPerSecond},
		{"medianFrameMicroseconds", medianFrameMicroseconds}, {"frameSamplesMicroseconds", frameSamplesMicroseconds},
		{"lifecycleCycles", options.cycles}, {"lifecycleRevocations", lifecycleRevocations}, {"lifecycleSamples", lifecycleSamples},
		{"invalidExplanations", invalidExplanations}, {"distinctInvalidExplanations", distinctInvalidExplanations.size()},
		{"workloadId", workloadId}};
	const nlohmann::json evidence = {
		{"schemaVersion", 1U}, {"identity", {
			{"buildSha256", buildHash}, {"sourceSha256", sourceHash}, {"settingsSha256", settingsHash},
			{"workloadSha256", workloadHash}, {"renderer", "TerraEngine"}, {"powerMode", "unknown"}, {"workerCount", options.agents}}},
		{"content", content}, {"contentSha256", Sha256Hex(std::string_view(content.dump()))},
		{"metrics", { {"routeLatencyMilliseconds", Percentile(latencyMicroseconds, 0.95) / 1000.0},
			{"dynamicPublicationResponsivenessMilliseconds", dynamicPublicationResponsivenessMilliseconds >= 0.0
				? nlohmann::json(dynamicPublicationResponsivenessMilliseconds) : nlohmann::json(nullptr)} }},
		{"lane", lane}, {"workloadId", workloadId}, {"success", success},
		{"denominator", {{"attempted", submitted}, {"completed", completed}, {"cancelled", cancelled},
			{"rejected", rejected}, {"failed", failed}, {"timedOut", timedOut}}},
		{"latency", {{"p95Milliseconds", Percentile(latencyMicroseconds, 0.95) / 1000.0},
			{"p99Milliseconds", Percentile(latencyMicroseconds, 0.99) / 1000.0}}},
		{"safety", {{"noCrash", true}, {"noDeadlock", true}, {"noStaleOwnerPublications", lifecycleFailures == 0U},
			{"removalSafety", dynamicRemovalReceiptsAccepted > 0U || std::find(options.matrices.begin(), options.matrices.end(), "soak") == options.matrices.end()},
			{"boundedStatus", rawSamples.size() <= options.rawSampleLimit}}}
	};
	std::error_code error;
	if(options.output.has_parent_path()) std::filesystem::create_directories(options.output.parent_path(), error);
	std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
	if(!output) return 4;
	output << evidence.dump(2);
	if(!output.good()) return 5;
	streaming.Shutdown();
	mutations.Clear();
	objects.clear();
	system.Shutdown();
	return success ? 0 : 1;
}
