// RawMeshStagingTests.cpp - raw package staging isolation and float round trips.
#include "TestHarness.h"
#include "MeshCompiler.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <iostream>
#include <thread>

namespace TerraEngine::EditorTests {
namespace {
	struct FixtureRoot {
		std::filesystem::path path;
		FixtureRoot() {
			static std::atomic<unsigned> serial{0};
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			for(unsigned attempt = 0; attempt != 512; ++attempt) {
				path = std::filesystem::temp_directory_path() / ("TerraRawStageTest-" + std::to_string(stamp) + "-" + std::to_string(serial.fetch_add(1)));
				std::error_code error;
				if(std::filesystem::create_directory(path, error)) return;
				if(error && error != std::errc::file_exists) throw std::runtime_error("Cannot create isolated raw staging fixture.");
			}
			throw std::runtime_error("Raw staging fixture directory reservation exhausted.");
		}
		~FixtureRoot() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
	};

	std::vector<RawMeshVertex> Triangle(float firstX, float secondX) {
		std::vector<RawMeshVertex> vertices(3);
		vertices[0].position = {firstX, 0.0f, 0.0f};
		vertices[1].position = {secondX, 0.0f, 0.0f};
		vertices[2].position = {firstX, 1.0f, 0.0f};
		for(auto& vertex : vertices) vertex.normal = {0.0f, 0.0f, 1.0f};
		vertices[0].uv = {0.0f, 0.0f}; vertices[1].uv = {1.0f, 0.0f}; vertices[2].uv = {0.0f, 1.0f};
		return vertices;
	}

	// The public decoder exposes quantization bounds independently of OBJ text.
	// Copy the values before clearing its cache; never retain a pointer across clears.
	std::array<float, 4> ReadBounds(TestContext& context, const std::filesystem::path& path) {
		auto& resources = ResourceManager::Instance();
		resources.ClearCaches();
		const auto id = resources.LoadMesh(path);
		const auto* mesh = resources.GetMesh(id);
		context.Expect(mesh != nullptr, "A completed raw compile must load through the production decoder.");
		const auto& q = mesh->runtimeLayout.quantization;
		const std::array<float, 4> bounds{q.positionOffset[0] - q.positionScale[0], q.positionOffset[0] + q.positionScale[0],
			q.uvOffset[0], q.uvOffset[0] + q.uvScale[0]};
		resources.ClearCaches();
		return bounds;
	}

	// Tests run sequentially in the registry. Compare the shared staging root's
	// contents before/after our calls so success and rejection cannot leave new
	// files or private directories behind; existing unrelated leftovers are kept.
	std::set<std::filesystem::path> StagingContents() {
		const auto root = std::filesystem::temp_directory_path() / "TerraEngineRawMeshCompile";
		std::set<std::filesystem::path> entries;
		if(std::filesystem::exists(root)) {
			for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) entries.insert(entry.path().lexically_relative(root));
		}
		return entries;
	}

	struct ImportCheckpoint {
		std::mutex mutex;
		std::condition_variable changed;
		bool entered = false;
		bool released = false;
		bool timedOut = false;
		std::thread::id producerThread;
		void Release() { { std::lock_guard lock(mutex); released = true; } changed.notify_all(); }
		void HoldOnce(float progress, std::string_view stage) {
			if(stage != "Mesh Import" || progress != 0.05f) return;
			std::unique_lock lock(mutex);
			if(entered) return;
			// This callback runs inside the real raw compiler, not a pool-capacity probe.
			producerThread = std::this_thread::get_id();
			entered = true; changed.notify_all();
			// This is a test-only checkpoint, never a runtime synchronization policy.
			if(!changed.wait_for(lock, std::chrono::seconds(30), [&] { return released; })) timedOut = true;
		}
	};
	struct ReleaseCheckpoint {
		ImportCheckpoint& checkpoint;
		~ReleaseCheckpoint() { checkpoint.Release(); }
	};

	void RunOverlap(TestContext& context, bool invalidSecond) {
		const auto ownerThread = std::this_thread::get_id();
		FixtureRoot fixture;
		const auto stagedBefore = StagingContents();
		const auto a = fixture.path / "a" / "SameName.temf";
		const auto b = fixture.path / "b" / "SameName.temf";
		std::filesystem::create_directory(a.parent_path());
		std::filesystem::create_directory(b.parent_path());
		const auto verticesA = Triangle(0.0f, 1.0f);
		const auto verticesB = Triangle(20.0f, 22.0f);
		const std::vector<std::uint32_t> indicesA{0, 1, 2};
		const std::vector<std::uint32_t> indicesB = invalidSecond ? std::vector<std::uint32_t>{0, 1, 99} : indicesA;
		// Declare checkpoint before its pool: pool destruction joins borrowers first.
		ImportCheckpoint checkpoint;
		auto pool = ThreadPool::CreateOwned(1);
		auto task = pool->createTask([&] {
			return CompileMeshFromRaw("RawStage/A.obj", a, verticesA, indicesA, true, nullptr, 60.0f, 0.0f, nullptr,
				[&](float progress, std::string_view stage, std::string_view) { checkpoint.HoldOnce(progress, stage); });
		});
		auto future = task->getFuture();
		// Release guard must outlive exceptions from B and assertions on the owner.
		ReleaseCheckpoint release{checkpoint};
		pool->enqueue(task);
		bool entered;
		{ std::unique_lock lock(checkpoint.mutex); entered = checkpoint.changed.wait_for(lock, std::chrono::seconds(30), [&] { return checkpoint.entered; }); }
		bool second = false;
		if(entered) second = CompileMeshFromRaw("RawStage/B.obj", b, verticesB, indicesB, true, nullptr, 60.0f, 0.0f);
		checkpoint.Release();
		const bool first = future.get();
		const auto stagedAfter = StagingContents();
		std::cout << "Raw staging real import identities: owner=" << ownerThread
			<< " producer=" << checkpoint.producerThread << " joined=" << first << '\n';
		// No throwing test assertion occurs while a worker is held at the checkpoint.
		context.Expect(checkpoint.producerThread != std::thread::id{} && checkpoint.producerThread != ownerThread
			&& std::this_thread::get_id() == ownerThread,
			"Actual raw import preparation must run on a foreign worker and return to its owner for decoded publication checks.");
		context.Expect(entered && !checkpoint.timedOut, "A must reach its pre-import checkpoint without timeout.");
		context.Expect(second == !invalidSecond, "B must have the intended valid or invalid-index outcome.");
		context.Expect(first, "B must neither replace nor remove A's private staging input.");
		context.Expect(stagedAfter == stagedBefore, "Both imports must remove their staging files/directories after success or rejection.");
		const auto boundsA = ReadBounds(context, a);
		context.Expect(std::fabs(boundsA[0]) < 0.001f && std::fabs(boundsA[1] - 1.0f) < 0.001f, "A must contain its own input geometry.");
		if(!invalidSecond) {
			const auto boundsB = ReadBounds(context, b);
			context.Expect(std::fabs(boundsB[0] - 20.0f) < 0.001f && std::fabs(boundsB[1] - 22.0f) < 0.001f, "B must contain its independent geometry.");
		}
	}
}

void RegisterRawMeshStagingTests(TestRegistry& registry) {
	registry.Add("Editor.AssetPipeline.RawStaging.ConcurrentEqualNamesStayIsolated", [](TestContext& context) { RunOverlap(context, false); });
	registry.Add("Editor.AssetPipeline.RawStaging.InvalidConcurrentInputPreservesOwner", [](TestContext& context) { RunOverlap(context, true); });
	registry.Add("Editor.AssetPipeline.RawStaging.LargeCoordinatesDoNotCollapse", [](TestContext& context) {
		FixtureRoot fixture;
		const auto output = fixture.path / "Precise.temf";
		auto vertices = Triangle(10000.125f, 10000.15625f);
		// Large adjacent UV values expose the same lossy text formatting as positions.
		vertices[0].uv[0] = vertices[2].uv[0] = 10000.125f;
		vertices[1].uv[0] = 10000.15625f;
		const std::vector<std::uint32_t> indices{0, 1, 2};
		const auto stagedBefore = StagingContents();
		const bool compiled = CompileMeshFromRaw("RawStage/Precise.obj", output, vertices, indices, true, nullptr, 60.0f, 0.0f);
		context.Expect(StagingContents() == stagedBefore, "The precision import must remove all private staging content.");
		context.Expect(compiled, "Distinct float vertices must not collapse in the temporary text round trip.");
		const auto bounds = ReadBounds(context, output);
		context.Expect(std::fabs(bounds[2] - 10000.125f) <= 0.002f && std::fabs(bounds[3] - 10000.15625f) <= 0.002f, "Decoded UV bounds must retain the original float values within format tolerance.");
		context.Expect(std::fabs(bounds[0] - 10000.125f) <= 0.002f && std::fabs(bounds[1] - 10000.15625f) <= 0.002f, "Decoded production bounds must preserve distinct source positions within float/quantization tolerance.");
	});
}
}
