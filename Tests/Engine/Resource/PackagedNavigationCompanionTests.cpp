// Actual archive-backed navigation reads and protected mesh source capture.
#include "TestHarness.h"
#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "TEPAK/PackWriter.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"
#include "Components/MeshRenderComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/ComponentRegistry.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace TerraEngine::Tests {
namespace {
	using namespace Navigation;
	NavigationGeometryDocument Triangle() {
		NavigationGeometryDocument document; document.kind = NavigationGeometryPayloadKind::TriangleMesh;
		document.vertices = {{0.0F,0.0F,0.0F},{2.0F,0.0F,0.0F},{0.0F,0.0F,2.0F}}; document.triangles = {{0U,1U,2U}}; return document;
	}
	struct Session {
		ResourceManager& resources = ResourceManager::Instance();
		std::filesystem::path oldAsset = resources.GetAssetRoot(), oldEngine = resources.GetEngineRoot(), root;
		std::vector<std::byte> bytes;
		bool ownsRoot = false;
		const TerraEngine::UUID id{74U,7U};
		const std::filesystem::path logical = "Nav/Triangle.tenavgeom";
		Session() {
			// This fixture owns global mount/resource state only while the sequential
			// registry runs; never replace a caller's already-mounted archive.
			if(ContentMount::Instance().IsMounted() || resources.IsExportRuntime()) throw std::runtime_error("Navigation fixture needs an unmounted editor baseline.");
			try {
			static std::atomic<unsigned> serial{0};
			root = std::filesystem::temp_directory_path() / ("TerraNavMount-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial.fetch_add(1)));
			if(!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot reserve navigation fixture root.");
			ownsRoot = true;
			std::filesystem::create_directories(root / "Content/Nav"); std::filesystem::create_directories(root / "Runtime");
			const auto encoded = EncodeNavigationGeometry(Triangle());
			if(!encoded.accepted) throw std::runtime_error("Cannot encode navigation fixture.");
			bytes = encoded.bytes;
			Write(root / "Content" / logical);
			tepak::PackagingRequest request; request.inputRoot = root / "Content"; request.outputPackPath = root / "Runtime/game.tepak";
			request.workerCount = 1U; request.extentSizingBytes = 4096U; request.enableCompression = true; request.enableDictionaryTraining = false; request.overwriteExisting = true;
			(void)tepak::PackWriter::Create(request);
			if(!ContentMount::Instance().Mount(request.outputPackPath)) throw std::runtime_error("Cannot mount navigation fixture archive.");
			resources.ClearCaches(); resources.ConfigureExportArchive(request.outputPackPath);
			std::filesystem::remove(root / "Content" / logical);
			resources.RegisterNavigationGeometryPath(id, logical);
			} catch(...) { Reset(); throw; }
		}
		~Session() { Reset(); }
		void Reset() {
			resources.ClearCaches(); ContentMount::Instance().Unmount(); resources.ConfigureExportArchive({}); resources.SetAssetRoot(oldAsset); resources.SetEngineRoot(oldEngine);
			std::error_code ignored; if(ownsRoot) std::filesystem::remove_all(root, ignored);
		}
		void Write(const std::filesystem::path& file) {
			std::filesystem::create_directories(file.parent_path()); std::ofstream out(file, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if(!out) throw std::runtime_error("Cannot write navigation fixture.");
		}
		NavigationGeometrySnapshotDescriptor Descriptor() const {
			NavigationGeometrySnapshotDescriptor d; d.sourceUuid.value = id; d.sourceRevision = 1U; d.canonicalPlacementRevision = 1U; d.physicalReadyRevision = 1U; return d;
		}
	};
}
void RegisterPackagedNavigationCompanionTests(TestRegistry& registry) {
	registry.Add("Resource.NavigationCompanion.ArchiveOnlyBytesReachProtectedCapture", [](TestContext& context) {
		Session session;
		context.Expect(!std::filesystem::exists(session.resources.ResolveAssetPath(session.logical)), "No loose runtime companion may mask the mounted read.");
		const auto loaded = session.resources.LoadRegisteredNavigationGeometryCompanion(session.id, session.Descriptor());
		context.Expect(loaded.accepted && loaded.snapshot && loaded.sourceBytes == session.bytes.size(), "A real mounted companion must load without a loose filesystem size probe.");
		ComponentRegistry::RegisterAllComponents(); auto& objects = ObjectManager::instance();
		const auto object = objects.createObject("Mounted companion source");
		struct RemoveOwnedObject { ObjectManager& manager; Object object; ~RemoveOwnedObject() { manager.removeObject(object); } } remove{objects, object};
		objects.AddComponent<Transform>(object);
		objects.AddComponent<MeshRenderComponent>(object)->meshId = session.id;
		// Test setup owns detached live state at the protected boundary. Capture
		// must retain the physical mesh source even when no terrain world exists.
		const auto captured = CaptureNavigationRuntimeScene(objects, session.resources, {}, {});
		context.Expect(captured.bootstrap.eligibleSourceIndex != nullptr, "Protected capture must publish its copied source index.");
		const auto& records = captured.bootstrap.eligibleSourceIndex->Records();
		context.Expect(std::any_of(records.begin(), records.end(), [&](const auto& record) { return record.identity.kind == NavigationSourceKind::Mesh && record.identity.assetId == session.id && record.geometry != nullptr; }),
			"Automatic exported capture must include the actual mounted mesh companion.");
		context.Expect(!captured.bootstrap.terrainAvailable, "This mesh-only fixture must preserve the independent missing-terrain capability boundary.");
	});
	registry.Add("Resource.NavigationCompanion.ArchiveEntryRejectsBeforeOversizeRead", [](TestContext& context) {
		Session session;
		const auto loaded = session.resources.LoadRegisteredNavigationGeometryCompanion(session.id, session.Descriptor(), session.bytes.size() - 1U);
		context.Expect(!loaded.accepted && loaded.reason == "NavigationGeometryByteLimitExceeded" && !loaded.snapshot,
			"Archive entry metadata must enforce the supplied byte limit before a whole-entry read.");
		context.Expect(!session.resources.GetNavigationGeometrySnapshot(session.id), "Rejected oversized input must not publish a partial navigation snapshot.");
	});
	registry.Add("Resource.NavigationCompanion.LooseEditorCompanionControl", [](TestContext& context) {
		Session session;
		// The mount-authority fix must preserve normal editor loose-file loading.
		ContentMount::Instance().Unmount(); session.resources.ConfigureExportArchive({});
		session.Write(session.resources.ResolveAssetPath(session.logical));
		const auto loaded = session.resources.LoadRegisteredNavigationGeometryCompanion(session.id, session.Descriptor());
		context.Expect(loaded.accepted && loaded.snapshot && !loaded.snapshot->Triangles().empty(), "The same checked geometry must remain loadable from an unmounted editor project.");
	});
	registry.Add("Resource.NavigationCompanion.MissingMountedEntryCannotUseLooseFallback", [](TestContext& context) {
		Session session; const auto missing = std::filesystem::path("Nav/Missing.tenavgeom"); session.Write(session.resources.ResolveAssetPath(missing));
		const auto loaded = session.resources.LoadNavigationGeometryCompanion(session.id, missing, session.Descriptor());
		context.Expect(!loaded.accepted && !loaded.snapshot, "An export-authoritative mounted miss must not consume a developer loose companion.");
	});
}
}
