// NavigationAgentPersistenceParityTests.cpp - editor/export authored-agent parity.
//
// The runtime cache is an optional acceleration artifact. It may change route
// startup cost, but it must not change the serialized authored component or
// reconstruct runtime intent when a scene is loaded from the exported archive.

#include "../TestHarness.h"

#include "NavigationExportDependencies.h"
#include "TEPAK/PackWriter.h"
#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/Persistence/SavedNavigation.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Scene/Serializer.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		enum class CacheVariant { Absent, Current, Stale };

		// An UUID-named staging root lets concurrent editor tests package and remove
		// their own archive without racing another export's filesystem cleanup.
		class NavigationAgentExportWorkspace final {
		public:
			NavigationAgentExportWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests"
					/ ("NavigationAgentExport-" + ToHexString(GenerateUUID()))) {
				std::filesystem::create_directories(StagingRoot());
			}

			~NavigationAgentExportWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path StagingRoot() const { return root_ / "Staging"; }
			[[nodiscard]] std::filesystem::path ArchivePath() const { return root_ / "Output" / "NavigationAgent.tepak"; }

		private:
			std::filesystem::path root_;
		};

		bool WriteBytes(const std::filesystem::path& destination, const std::vector<std::byte>& bytes) {
			std::error_code error;
			std::filesystem::create_directories(destination.parent_path(), error);
			if(error) return false;
			std::ofstream output(destination, std::ios::binary | std::ios::trunc);
			if(!output) return false;
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return output.good();
		}

		bool WriteJson(const std::filesystem::path& destination, const nlohmann::json& document) {
			std::error_code error;
			std::filesystem::create_directories(destination.parent_path(), error);
			if(error) return false;
			std::ofstream output(destination, std::ios::binary | std::ios::trunc);
			if(!output) return false;
			output << document.dump(2);
			return output.good();
		}

		Navigation::SavedNavigationDescriptor CacheDescriptor() {
			Navigation::SavedNavigationDescriptor descriptor{};
			descriptor.formatVersion = Navigation::kSavedNavigationFormatVersion;
			descriptor.backendAbi = std::string(Navigation::kSavedNavigationBackendAbi);
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{0.0F, 0.0F, 0.0F}, {8.0F, 0.0F, 8.0F}, {8.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			Navigation::NavigationGeometrySnapshotDescriptor sourceDescriptor{};
			sourceDescriptor.sourceRevision = 1U;
			sourceDescriptor.canonicalPlacementRevision = 1U;
			sourceDescriptor.physicalReadyRevision = 1U;
			sourceDescriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			const auto source = Navigation::NavigationGeometrySnapshot::Create(sourceDescriptor, document);
			Navigation::RecastGroundBuildInput input{};
			input.profile = Navigation::NavigationProfile::Defaults();
			if(source.snapshot) input.sources = {source.snapshot};
			input.maximumTiles = 4U;
			const auto evidence = Navigation::CaptureGroundCoverageEvidence(input);
			descriptor.sourceFingerprint = evidence.sourceFingerprint;
			descriptor.settingsFingerprint = evidence.settingsFingerprint;
			return descriptor;
		}

		bool WriteGeometryCompanion(const std::filesystem::path& stagingRoot) {
			Navigation::NavigationGeometryDocument geometry{};
			geometry.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			geometry.vertices = {{0.0F, 0.0F, 0.0F}, {8.0F, 0.0F, 8.0F},
				{8.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 8.0F}};
			geometry.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			const Navigation::NavigationGeometryEncodeResult encoded = Navigation::EncodeNavigationGeometry(geometry);
			return encoded.accepted && WriteBytes(stagingRoot / "Navigation" / "ground.tenavgeom", encoded.bytes);
		}

		bool WriteOptionalCache(const std::filesystem::path& stagingRoot, const Navigation::SavedNavigationDescriptor& descriptor) {
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{0.0F, 0.0F, 0.0F}, {8.0F, 0.0F, 8.0F}, {8.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			Navigation::NavigationGeometrySnapshotDescriptor sourceDescriptor{};
			sourceDescriptor.sourceRevision = 1U;
			sourceDescriptor.canonicalPlacementRevision = 1U;
			sourceDescriptor.physicalReadyRevision = 1U;
			sourceDescriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			const auto source = Navigation::NavigationGeometrySnapshot::Create(sourceDescriptor, document);
			Navigation::RecastGroundBuildInput buildInput{};
			buildInput.profile = Navigation::NavigationProfile::Defaults();
			if(!source.snapshot) return false;
			buildInput.sources = {source.snapshot};
			buildInput.maximumTiles = 4U;
			const auto built = Navigation::BuildRecastGroundTiles(buildInput);
			if(!built.Succeeded() || built.tiles.empty()) return false;
			const auto exported = Navigation::BuildSavedNavigationChunks(built.tiles);
			if(!exported.accepted) return false;
			return Navigation::WriteSavedNavigationCacheAtomically(
				stagingRoot / "Navigation" / "ground.tenav", descriptor, exported.chunks).accepted;
		}

		struct ExportedAgentScenario final {
			bool packageCreated = false;
			bool manifestCreated = false;
			Runtime::PackagedNavigationRuntime runtime{};
			Navigation::SavedNavigationReadResult cacheRead{};
			bool authoredValuesLoaded = false;
			bool runtimeStartsIdle = false;
			bool guidanceUsable = true;
		};

		ExportedAgentScenario PackageAndReloadAgent(CacheVariant cacheVariant) {
			NavigationAgentExportWorkspace workspace;
			ExportedAgentScenario result{};
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			Object authored = objects.createObject("Exported Navigation Agent");
			(void)objects.AddComponent<Transform>(authored);
			NavigationAgentComponent* component = objects.AddComponent<NavigationAgentComponent>(authored);
			if(component == nullptr) return result;
			component->radiusMetres = 0.65;
			component->heightMetres = 2.10;
			component->areaPenalties[3U] = 2.50;
			component->arrivalToleranceMetres = 0.20;
			// ObjectManager::clear below invalidates component pointers. Preserve the
			// authored values as plain copies before the exported scene reload begins.
			const double authoredRadius = component->radiusMetres;
			const double authoredHeight = component->heightMetres;
			const double authoredAreaPenalty = component->areaPenalties[3U];
			const double authoredArrivalTolerance = component->arrivalToleranceMetres;

			const nlohmann::json scene = {
				{"format", Scene::kTerraSceneUnboundedFormat},
				{"objects", nlohmann::json::array({Serializer::SerializeObject(authored)})}
			};
			const bool sceneWritten = WriteJson(workspace.StagingRoot() / "Scenes" / "Agent.tescene", scene);
			const bool gameManifestWritten = WriteJson(workspace.StagingRoot() / "game.json", {
				{"startupScene", "Scenes/Agent.tescene"}
			});
			const bool geometryWritten = WriteGeometryCompanion(workspace.StagingRoot());
			const Navigation::SavedNavigationDescriptor descriptor = CacheDescriptor();
			const bool cacheWritten = cacheVariant == CacheVariant::Absent
				|| WriteOptionalCache(workspace.StagingRoot(), descriptor);
			const Export::NavigationExportClosure closure = Export::NavigationExportDependencyResolver::Resolve(workspace.StagingRoot());
			std::string manifestError;
			result.manifestCreated = sceneWritten && gameManifestWritten && geometryWritten && cacheWritten
				&& closure.Complete() && Export::NavigationExportDependencyResolver::WriteRuntimeManifest(
					closure, workspace.StagingRoot(), manifestError);
			if(!result.manifestCreated) {
				objects.clear();
				return result;
			}

			tepak::PackagingRequest request{};
			request.inputRoot = workspace.StagingRoot();
			request.outputPackPath = workspace.ArchivePath();
			request.extentSizingBytes = 4096U;
			request.workerCount = 1U;
			request.enableCompression = false;
			request.enableDictionaryTraining = false;
			request.overwriteExisting = true;
			request.peakMemoryBudgetBytes = 16ULL * 1024ULL * 1024ULL;
			std::filesystem::create_directories(workspace.ArchivePath().parent_path());
			const tepak::PackWriteResult packaged = tepak::PackWriter::Create(request);
			result.packageCreated = packaged.descriptor.fileCount > 0U && std::filesystem::exists(workspace.ArchivePath());
			if(!result.packageCreated) {
				objects.clear();
				return result;
			}

			ContentMount mountedExport;
			if(!mountedExport.Mount(workspace.ArchivePath())) {
				objects.clear();
				return result;
			}
			result.runtime = Runtime::LoadPackagedNavigationRuntime(mountedExport);
			if(cacheVariant != CacheVariant::Absent) {
				const std::vector<std::uint8_t> cacheBytes = mountedExport.ReadAll("Navigation/ground.tenav");
				std::vector<std::byte> typedCache(cacheBytes.size());
				for(std::size_t index = 0U; index < cacheBytes.size(); ++index)
					typedCache[index] = static_cast<std::byte>(cacheBytes[index]);
				Navigation::SavedNavigationDescriptor expected = descriptor;
				if(cacheVariant == CacheVariant::Stale) ++expected.settingsFingerprint;
				result.cacheRead = Navigation::ReadSavedNavigation(typedCache, expected);
			}

			const std::vector<std::uint8_t> sceneBytes = mountedExport.ReadAll("Scenes/Agent.tescene");
			if(!sceneBytes.empty()) {
				const nlohmann::json exportedScene = nlohmann::json::parse(sceneBytes.begin(), sceneBytes.end());
				objects.clear();
				Object loaded = Serializer::DeserializeObject(exportedScene.at("objects").at(0U));
				NavigationAgentComponent* loadedComponent = objects.GetComponent<NavigationAgentComponent>(loaded);
				result.authoredValuesLoaded = loadedComponent != nullptr
					&& loadedComponent->radiusMetres == authoredRadius
					&& loadedComponent->heightMetres == authoredHeight
					&& loadedComponent->areaPenalties[3U] == authoredAreaPenalty
					&& loadedComponent->arrivalToleranceMetres == authoredArrivalTolerance;

				Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
				Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
				navigation.Shutdown();
				agents.Shutdown();
				navigation.Initialize();
				agents.Initialize();
				const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(loaded));
				const Navigation::NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);
				const Navigation::NavigationAgentGuidanceSnapshot guidance = agents.ReadGuidance(identity);
				result.runtimeStartsIdle = identity.IsValid() && status.state == Navigation::NavigationAgentState::Idle;
				result.guidanceUsable = guidance.IsUsable();
				agents.Shutdown();
				navigation.Shutdown();
			}
			mountedExport.Unmount();
			objects.clear();
			return result;
		}
	}

	void RegisterNavigationAgentPersistenceParityTests(TestRegistry& registry) {
		registry.Add("Editor.Navigation.AgentPersistence.ExportedArchivePreservesAuthoringAcrossCacheVariants", [](TestContext& context) {
			const ExportedAgentScenario cached = PackageAndReloadAgent(CacheVariant::Current);
			const ExportedAgentScenario uncached = PackageAndReloadAgent(CacheVariant::Absent);
			const ExportedAgentScenario stale = PackageAndReloadAgent(CacheVariant::Stale);

			const auto preservedAuthoring = [](const ExportedAgentScenario& scenario) {
				return scenario.packageCreated && scenario.manifestCreated && scenario.runtime.loaded
					&& scenario.runtime.sourceFree && scenario.runtime.geometryPaths.size() == 1U
					&& scenario.authoredValuesLoaded && scenario.runtimeStartsIdle && !scenario.guidanceUsable;
			};
			context.Expect(preservedAuthoring(cached) && preservedAuthoring(uncached) && preservedAuthoring(stale),
				"A real mounted export archive must deserialize identical authored NavigationAgent settings and fresh Idle runtime state with current, absent, or stale optional cache artifacts.");
			context.Expect(cached.runtime.optionalCachePaths.size() == 1U && cached.cacheRead.accepted
				&& cached.cacheRead.matchesFreshRuntimeGeneration,
				"A current optional cache must survive staging, packaging, archive mounting, and its checked derived-data read.");
			context.Expect(cached.runtime.optionalCacheObservations.size() == 1U
				&& cached.runtime.optionalCacheObservations.front().present
				&& !cached.runtime.optionalCacheObservations.front().accepted
				&& cached.runtime.optionalCacheObservations.front().reason == "present_deferred_until_authoritative_descriptor",
				"Startup must observe a mounted current cache but defer acceptance until authoritative source/settings fingerprints are available.");
			context.Expect(uncached.runtime.optionalCachePaths.empty() && !uncached.cacheRead.accepted,
				"A cache-free export must remain a valid source-free package and use the same authored scene payload.");
			context.Expect(uncached.runtime.optionalCacheObservations.empty(),
				"A cache-free export must report no optional cache observations and continue with fresh generation.");
			context.Expect(stale.runtime.optionalCachePaths.size() == 1U && !stale.cacheRead.accepted,
				"A stale optional cache may be packaged for startup probing, but its checked read must reject it without changing the loaded agent's authored values or runtime intent.");
			context.Expect(stale.runtime.optionalCacheObservations.size() == 1U
				&& stale.runtime.optionalCacheObservations.front().present
				&& !stale.runtime.optionalCacheObservations.front().accepted
				&& stale.runtime.optionalCacheObservations.front().reason == "present_deferred_until_authoritative_descriptor",
				"Startup must fail open for a present stale cache rather than trusting its self-declared fingerprint.");
		});

		registry.Add("Editor.Navigation.AgentPersistence.ReloadStartsIdleWithOrWithoutCache", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object authored = objects.createObject("Reloaded Navigation Agent");
			(void)objects.AddComponent<Transform>(authored);
			(void)objects.AddComponent<NavigationAgentComponent>(authored);
			const nlohmann::json payload = Serializer::SerializeObject(authored);
			objects.clear();
			const Object reloaded = Serializer::DeserializeObject(payload);
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			agents.Shutdown();
			navigation.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const auto identity = agents.FindIdentity(objects, objects.getUUID(reloaded));
			const auto status = agents.ReadStatus(identity);
			const auto guidance = agents.ReadGuidance(identity);
			context.Expect(identity.IsValid() && status.state == Navigation::NavigationAgentState::Idle
				&& !guidance.IsUsable(),
				"Reloading authored data must begin Idle with no destination or usable guidance, independent of cache presence.");
			agents.Shutdown();
			navigation.Shutdown();
			objects.clear();
		});
	}
}
