// SavedNavigationTests.cpp - provider-neutral saved navigation cache contract.
// The saved-navigation implementation must provide the headers named below; this test intentionally
// establishes the durable API before any cache reader/writer is implemented.

#include "TestHarness.h"
#include "Components/NavigationAgentComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Persistence/SavedNavigation.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Backend/DetourGroundShard.h"
#include "Scene/Serializer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::Tests {
	namespace {
		[[nodiscard]] std::uint64_t SavedTileChecksum(std::span<const std::byte> bytes) {
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::byte value : bytes) {
				hash ^= std::to_integer<unsigned char>(value);
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		[[nodiscard]] Navigation::SavedNavigationChunk MakeSavedChunk(
			Navigation::NavigationCoverageKey key, bool empty) {
			Navigation::SavedNavigationChunk chunk{};
			chunk.key = key;
			chunk.generation = 9U;
			chunk.sourceFingerprints = {3U, 7U};
			chunk.bounds = Navigation::CanonicalGroundTileBounds({
				key.shardX, key.shardZ,
				key.verticalBand, key.profileBuildKey});
			chunk.empty = empty;
			chunk.supportComplete = true;
			chunk.clearanceComplete = true;
			if(!empty) chunk.bytes = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
			chunk.checksum = SavedTileChecksum(chunk.bytes);
			return chunk;
		}
	}

	void RegisterSavedNavigationTests(TestRegistry& registry) {
		registry.Add("Navigation.SavedNavigation.ProfileValuesHaveNoIdentityOrRevision", [](TestContext& context) {
			const Navigation::NavigationProfile profile = Navigation::NavigationProfile::Defaults();
			const nlohmann::json serialized = Serializer::SerializeNavigationProfile(profile);
			context.Expect(serialized.value("schemaVersion", 0U) == 1U
				&& !serialized.contains("profileId") && !serialized.contains("revision"),
				"The hard-break profile schema must persist only traversal values.");
			context.Expect(Serializer::DeserializeNavigationProfile(serialized).has_value(),
				"The identity-free profile schema must round-trip its validated values.");

			nlohmann::json legacy = serialized;
			legacy["schemaVersion"] = 1U;
			legacy["profileId"] = 1U;
			legacy["revision"] = 1U;
			context.Expect(!Serializer::DeserializeNavigationProfile(legacy).has_value(),
				"The removed identity-bearing profile schema must reject instead of receiving compatibility translation.");
		});

		registry.Add("Navigation.SavedNavigation.RejectsStaleCorruptAndIncompatibleCachesAtomically", [](TestContext& context) {
			Navigation::SavedNavigationDescriptor descriptor{};
			descriptor.formatVersion = Navigation::kSavedNavigationFormatVersion;
			descriptor.backendAbi = std::string(Navigation::kSavedNavigationBackendAbi);
			descriptor.sourceFingerprint = 17U;
			descriptor.settingsFingerprint = 19U;
			const std::array chunks{
				MakeSavedChunk({2, -3, 1, 77U}, false),
				MakeSavedChunk({3, -3, 1, 77U}, true)};
			const Navigation::SavedNavigationWriteResult written = Navigation::WriteSavedNavigationAtomically(descriptor, chunks);
			context.Expect(written.accepted, "A checked temporary write must replace its final cache atomically.");
			const Navigation::SavedNavigationReadResult fresh = Navigation::ReadSavedNavigation(written.bytes, descriptor);
			context.Expect(fresh.accepted && fresh.matchesFreshRuntimeGeneration && fresh.chunks.size() == 2U
				&& !fresh.chunks.front().empty && fresh.chunks.front().sourceFingerprints == std::vector<std::uint64_t>{3U, 7U}
				&& fresh.chunks.back().empty && fresh.chunks.back().bytes.empty()
				&& fresh.chunks.back().supportComplete && fresh.chunks.back().clearanceComplete,
				"A compatible v2 cache must preserve complete source evidence and authoritative empty tiles.");
			Navigation::SavedNavigationDescriptor stale = descriptor; stale.settingsFingerprint++;
			context.Expect(!Navigation::ReadSavedNavigation(written.bytes, stale).accepted,
				"Backend ABI, source, settings, version, bounds, and checksum mismatches must reject before publication.");
			Navigation::SavedNavigationDescriptor legacyProvider = descriptor;
			legacyProvider.backendAbi = "recast-1.6.0-dtpolyref64";
			context.Expect(!Navigation::ReadSavedNavigation(written.bytes, legacyProvider).accepted,
				"A cache produced for the retired global-float provider ABI must miss rather than reinterpret shard-local tile bytes.");

			std::vector<std::byte> corrupt = written.bytes;
			corrupt.back() ^= std::byte{0xff};
			context.Expect(!Navigation::ReadSavedNavigation(corrupt, descriptor).accepted,
				"A corrupt payload must reject before any decoded chunk becomes visible.");
			std::vector<std::byte> truncated(written.bytes.begin(), written.bytes.end() - 1);
			context.Expect(!Navigation::ReadSavedNavigation(truncated, descriptor).accepted,
				"A truncated chunk range must reject as one complete cache, never as a partial navigation root.");

			std::vector<std::byte> versionOne = written.bytes;
			versionOne[5U] = std::byte{'0'};
			versionOne[6U] = std::byte{'0'};
			versionOne[7U] = std::byte{'1'};
			const Navigation::SavedNavigationReadResult retired = Navigation::ReadSavedNavigation(versionOne, descriptor);
			context.Expect(!retired.accepted && retired.reason == "SavedNavigationUnsupportedVersion" && retired.chunks.empty(),
				"V1 cache bytes must be rejected atomically so callers use normal fresh generation.");
		});

		registry.Add("Navigation.SavedNavigation.RequiresCurrentCompleteGeometryEvidenceBeforeTileImport", [](TestContext& context) {
			Navigation::SavedNavigationDescriptor descriptor{};
			descriptor.backendAbi = std::string(Navigation::kSavedNavigationBackendAbi);
			descriptor.sourceFingerprint = 17U;
			descriptor.settingsFingerprint = 19U;
			const std::array chunks{MakeSavedChunk({2, -3, 1, 19U}, false)};
			const Navigation::SavedNavigationWriteResult written = Navigation::WriteSavedNavigationAtomically(descriptor, chunks);
			const Navigation::SavedNavigationReadResult read = Navigation::ReadSavedNavigation(written.bytes, descriptor);
			Navigation::GroundCoverageEvidence evidence{};
			evidence.accepted = true;
			evidence.sourceFingerprint = descriptor.sourceFingerprint;
			evidence.settingsFingerprint = descriptor.settingsFingerprint;
			evidence.sourceFingerprints = {3U, 7U};
			evidence.tileKeys = {{2, -3, 1, 19U}};
			const Navigation::SavedGroundTileImportResult imported = Navigation::ImportSavedGroundTiles(read, evidence);
			context.Expect(imported.accepted && imported.tiles.size() == 1U
				&& imported.tiles.front().pristineBytes == chunks.front().bytes,
				"A current complete cache imports only after exact freshly captured key and source evidence match.");

			evidence.sourceFingerprints = {7U};
			const Navigation::SavedGroundTileImportResult staleSources = Navigation::ImportSavedGroundTiles(read, evidence);
			context.Expect(!staleSources.accepted && staleSources.tiles.empty(),
				"A source-evidence mismatch must discard every candidate tile rather than partially hydrate a root.");

			Navigation::SavedNavigationChunk incomplete = chunks.front();
			incomplete.clearanceComplete = false;
			const std::array incompleteChunks{incomplete};
			context.Expect(!Navigation::WriteSavedNavigationAtomically(descriptor, incompleteChunks).accepted,
				"The writer must refuse incomplete support or clearance coverage before an optional cache exists.");

			Navigation::SavedNavigationChunk malformed = chunks.front();
			malformed.bytes.assign(64U, std::byte{0});
			malformed.checksum = SavedTileChecksum(malformed.bytes);
			const std::array malformedChunks{malformed};
			const Navigation::SavedNavigationWriteResult malformedWrite = Navigation::WriteSavedNavigationAtomically(
				descriptor, malformedChunks);
			const Navigation::SavedNavigationReadResult malformedRead = Navigation::ReadSavedNavigation(
				malformedWrite.bytes, descriptor);
			const Navigation::SavedGroundTileImportResult malformedImport = Navigation::ImportSavedGroundTiles(
				malformedRead, Navigation::GroundCoverageEvidence{true, {}, descriptor.sourceFingerprint,
					descriptor.settingsFingerprint, {3U, 7U}, {{2, -3, 1, 19U}}});
			Navigation::DetourGroundShardBuildInput shardInput{};
			shardInput.shard = {0, -1, 1, 19U};
			shardInput.expectedGeneration = 1U;
			shardInput.tiles = malformedImport.tiles;
			const Navigation::DetourGroundShardBuildResult malformedShard = Navigation::AssembleDetourGroundShard(shardInput);
			Navigation::DetourGroundShardBuildInput overBudgetInput = shardInput;
			overBudgetInput.maximumBytes = 1U;
			const Navigation::DetourGroundShardBuildResult overBudgetShard = Navigation::AssembleDetourGroundShard(overBudgetInput);
			context.Expect(malformedWrite.accepted && malformedRead.accepted && malformedImport.accepted
				&& !malformedShard.accepted && malformedShard.reason.starts_with("DetourCachedTile")
				&& !overBudgetShard.accepted && overBudgetShard.reason == "ShardTileInvalidOrBudgetExceeded",
				"Checksummed container bytes must fail provider validation and an oversized tile must fail budget admission before subtraction can underflow.");
		});

		registry.Add("Navigation.SavedNavigation.SourceRoleChangesInvalidateIdenticalGeometryCaches", [](TestContext& context) {
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F},
				{8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			Navigation::NavigationGeometrySnapshotDescriptor terrainDescriptor{};
			terrainDescriptor.sourceRevision = 1U;
			terrainDescriptor.canonicalPlacementRevision = 1U;
			terrainDescriptor.physicalReadyRevision = 1U;
			terrainDescriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			Navigation::NavigationGeometrySnapshotDescriptor colliderDescriptor = terrainDescriptor;
			colliderDescriptor.sourceKind = Navigation::NavigationSourceKind::Collider;
			const auto terrain = Navigation::NavigationGeometrySnapshot::Create(terrainDescriptor, document);
			const auto collider = Navigation::NavigationGeometrySnapshot::Create(colliderDescriptor, document);
			Navigation::RecastGroundBuildInput terrainInput{};
			terrainInput.profile = Navigation::NavigationProfile::Defaults();
			terrainInput.sources = {terrain.snapshot};
			Navigation::RecastGroundBuildInput colliderInput = terrainInput;
			colliderInput.sources = {collider.snapshot};
			const Navigation::GroundCoverageEvidence terrainEvidence = Navigation::CaptureGroundCoverageEvidence(terrainInput);
			const Navigation::GroundCoverageEvidence colliderEvidence = Navigation::CaptureGroundCoverageEvidence(colliderInput);
			context.Expect(terrain.accepted && collider.accepted && terrain.snapshot->ContentFingerprint() == collider.snapshot->ContentFingerprint()
				&& terrainEvidence.accepted && colliderEvidence.accepted
				&& terrainEvidence.sourceFingerprints != colliderEvidence.sourceFingerprints
				&& terrainEvidence.sourceFingerprint != colliderEvidence.sourceFingerprint,
				"Stable source-role evidence must distinguish equal geometry used as support from the same geometry used as a collider.");

			if(!terrainEvidence.accepted || terrainEvidence.tileKeys.empty()) return;
			Navigation::SavedNavigationDescriptor descriptor{};
			descriptor.backendAbi = std::string(Navigation::kSavedNavigationBackendAbi);
			descriptor.sourceFingerprint = terrainEvidence.sourceFingerprint;
			descriptor.settingsFingerprint = terrainEvidence.settingsFingerprint;
			Navigation::SavedNavigationChunk chunk = MakeSavedChunk({
				terrainEvidence.tileKeys.front().x, terrainEvidence.tileKeys.front().z,
				terrainEvidence.tileKeys.front().verticalBand, terrainEvidence.tileKeys.front().profileBuildKey}, false);
			chunk.sourceFingerprints = terrainEvidence.sourceFingerprints;
			const std::array chunks{chunk};
			const Navigation::SavedNavigationWriteResult written = Navigation::WriteSavedNavigationAtomically(descriptor, chunks);
			Navigation::SavedNavigationDescriptor colliderExpected = descriptor;
			colliderExpected.sourceFingerprint = colliderEvidence.sourceFingerprint;
			const Navigation::SavedNavigationReadResult roleChanged = Navigation::ReadSavedNavigation(written.bytes, colliderExpected);
			context.Expect(written.accepted && !roleChanged.accepted && roleChanged.reason == "SavedNavigationCompatibilityMismatch",
				"A support-to-collider reclassification must reject a cache before its provider tiles can be imported.");
		});

		registry.Add("Navigation.SavedNavigation.UnsupportedPrefabComponentsRejectBeforePublication", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object activeObject = objects.createObject("Active Scene Must Survive");
			const std::uint64_t activeGeneration = objects.GetSceneGeneration();
			const std::array<std::string_view, 2U> unsupportedComponents{
				"NavigationSourceComponent", "NavigationConnectionComponent"};

			for(const std::string_view componentName : unsupportedComponents) {
				// Each prefab is decoded into detached value records first. Unknown
				// component names must reject that complete candidate without exposing
				// a valid prefix to the protected publication boundary.
				nlohmann::json prefab = {
					{"format", std::string(TerraEngine::Scene::kTerraPrefabUnboundedFormat)},
					{"sourceId", "hard-break-negative-fixture"},
					{"objects", nlohmann::json::array({{
						{"uuid", "580000000000000000000000000010"},
						{"name", "Unsupported Prefab Object"},
						{"components", {{std::string(componentName), nlohmann::json::object()}}}
					}})}
				};
				const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PreparePrefabDecode(prefab);
				context.Expect(!prepared.accepted && prepared.objects.empty()
					&& prepared.objectCount == 0U && prepared.componentCount == 0U,
					"An unknown prefab component must reject the complete detached candidate.");
				context.Expect(prepared.reason.find("unregistered component") != std::string::npos,
					"Prefab rejection must use the ordinary unknown-component diagnostic.");
				context.Expect(objects.isObjectValid(activeObject) && objects.GetSceneGeneration() == activeGeneration,
					"Rejected prefab decoding must not mutate the live scene or its generation.");
			}

			// The agent is an ordinary registered component. Its authored values may
			// load through the normal detached decode path; runtime destinations and
			// route ownership are created later by the navigation agent system.
			nlohmann::json agentPrefab = {
				{"format", std::string(TerraEngine::Scene::kTerraPrefabUnboundedFormat)},
				{"sourceId", "agent-prefab-fixture"},
				{"objects", nlohmann::json::array({{
					{"uuid", "580000000000000000000000000011"},
					{"name", "Navigation Agent Object"},
					{"components", {{"NavigationAgentComponent", nlohmann::json::object()}}}
				}})}
			};
			const TerraEngine::Scene::PreparedSceneDecode agentPrepared = Serializer::PreparePrefabDecode(agentPrefab);
			context.Expect(agentPrepared.accepted && agentPrepared.objectCount == 1U
				&& agentPrepared.componentCount == 1U,
				"NavigationAgentComponent must use ordinary supported prefab loading without a legacy/schema gate.");
			context.Expect(objects.isObjectValid(activeObject) && objects.GetSceneGeneration() == activeGeneration,
				"Detached agent prefab validation must not mutate the existing live scene.");
			objects.clear();
		});

		registry.Add("Navigation.SavedNavigation.AgentPersistsAuthoredValuesOnly", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Persisted Navigation Agent");
			NavigationAgentComponent* component = objects.AddComponent<NavigationAgentComponent>(object);
			if(component != nullptr) {
				component->radiusMetres = 0.65;
				component->arrivalToleranceMetres = 0.2;
			}
			const nlohmann::json saved = Serializer::SerializeObject(object);
			const nlohmann::json agent = saved["components"]["NavigationAgentComponent"];
			const std::array<std::string_view, 9U> runtimeOnly{"destination", "guidance", "route", "intentSequence",
				"receipts", "requestHandle", "runtimeSlot", "publicationGeneration", "routeGeneration"};
			bool runtimeAbsent = true;
			for(const std::string_view key : runtimeOnly) runtimeAbsent = runtimeAbsent && !agent.contains(key);
			context.Expect(component != nullptr && agent.value("radiusMetres", 0.0) == 0.65
				&& agent.value("arrivalToleranceMetres", 0.0) == 0.2 && runtimeAbsent,
				"Scene serialization must retain authored agent settings while excluding runtime destination, route, and intent state.");

			// Drop the original object before loading so this exercises detached
			// authored reconstruction rather than mutating/reusing a live UUID.
			objects.clear();
			const Object restored = Serializer::DeserializeObject(saved);
			const NavigationAgentComponent* restoredComponent = objects.GetComponent<NavigationAgentComponent>(restored);
			context.Expect(restoredComponent != nullptr && restoredComponent->radiusMetres == 0.65
				&& restoredComponent->arrivalToleranceMetres == 0.2,
				"Ordinary scene loading must restore validated authored agent defaults without reconstructing runtime state.");
			objects.clear();
		});
	}
}
