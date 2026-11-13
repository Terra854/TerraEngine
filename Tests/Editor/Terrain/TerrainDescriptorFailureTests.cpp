// TerrainDescriptorFailureTests.cpp -- Rejected metadata and worker results stay detached.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainCreationService.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainPrecommitJob.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct DescriptorFixture
		{
			std::filesystem::path directory = std::filesystem::temp_directory_path()
				/ ("TerrainDescriptor-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			std::filesystem::path descriptor = directory / "owned.teterrain";
			TerraEngine::Editor::Terrain::TerrainCreationService service;
			nlohmann::json valid;
			DescriptorFixture()
			{
				TerraEngine::AssetPipeline::TerrainImportRequest request;
				request.assetId = TerraEngine::GenerateUUID();
				const auto created = service.CreateOrImport(request, descriptor);
				if(!created.succeeded) throw std::runtime_error(created.message);
				std::ifstream input(descriptor, std::ios::binary);
				input >> valid;
			}
			~DescriptorFixture()
			{
				std::error_code ignored;
				std::filesystem::remove_all(directory, ignored);
			}
			void Write(const nlohmann::json& value)
			{
				std::ofstream output(descriptor, std::ios::binary | std::ios::trunc);
				output << value.dump(); output.close();
				if(!output) throw std::runtime_error("Owned descriptor fixture write failed.");
			}
			void RejectThenRecover(TestContext& context, const nlohmann::json& malformed)
			{
				Write(malformed);
				const auto rejected = service.OpenExisting(descriptor);
				context.Expect(!rejected.succeeded && !rejected.message.empty() && !rejected.store
					&& rejected.assetId == TerraEngine::EMPTY_UUID && rejected.areaSnapshots.empty()
					&& rejected.heightfield.heightsMm.empty(),
					"Malformed metadata must return a diagnostic before exposing any root/store state.");
				Write(valid);
				const auto recovered = service.OpenExisting(descriptor);
				context.Expect(recovered.succeeded && recovered.store && recovered.compatibilityHeightfieldComplete,
					"The same persisted bundle must reopen after every rejected descriptor.");
			}
		};
	}

	void RegisterTerrainDescriptorFailureTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainDescriptor.MalformedDimensionsRejectAndRecover", [](TestContext& context) {
			DescriptorFixture fixture;
			const std::vector<nlohmann::json> invalid{nullptr, true, "256", nlohmann::json::array(),
				nlohmann::json::object(), -1, 0, 1.5, std::uint64_t{1} << 32u,
				(std::numeric_limits<std::uint64_t>::max)()};
			for(const auto* field : {"width", "height"}) for(const auto& value : invalid)
			{
				auto descriptor = fixture.valid; descriptor[field] = value;
				fixture.RejectThenRecover(context, descriptor);
			}
		});
		registry.Add("Editor.TerrainDescriptor.MalformedSpacingRejectsAndLegacyDefaultRecovers", [](TestContext& context) {
			DescriptorFixture fixture;
			for(const auto& value : std::vector<nlohmann::json>{nullptr, false, "1", nlohmann::json::array(),
				nlohmann::json::object(), -1.0, 0.0, 1.0e100, (std::numeric_limits<double>::min)()})
			{
				auto descriptor = fixture.valid; descriptor["sampleSpacingMetres"] = value;
				fixture.RejectThenRecover(context, descriptor);
			}
			// Only an absent spacing field retains the established one-metre default.
			auto legacy = fixture.valid; legacy.erase("sampleSpacingMetres"); fixture.Write(legacy);
			const auto opened = fixture.service.OpenExisting(fixture.descriptor);
			context.Expect(opened.succeeded && opened.sampleSpacingMetres == 1.0F,
				"Omitted legacy spacing must retain its valid documented default.");
		});
		registry.Add("Editor.TerrainDescriptor.WrappedVersionRejectsAndRecovers", [](TestContext& context) {
			DescriptorFixture fixture;
			for(const auto& value : std::vector<nlohmann::json>{nullptr, true, "1", -1, 1.5,
				(std::uint64_t{1} << 32u) + 1u})
			{
				auto descriptor = fixture.valid; descriptor["version"] = value;
				fixture.RejectThenRecover(context, descriptor);
			}
		});
		registry.Add("Editor.TerrainPrecommit.WorkerExceptionsRejectAndRecover", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1u);
			std::string error;
			const auto rejected = TerraEngine::Editor::Terrain::RunTerrainPrecommitJob(*pool,
				[]() -> int { throw std::runtime_error("owned terrain worker failure"); }, 1u, error);
			context.Expect(!rejected && error.find("owned terrain worker failure") != std::string::npos,
				"A real worker exception must become a rejected result with its diagnostic.");
			const auto unknown = TerraEngine::Editor::Terrain::RunTerrainPrecommitJob(*pool,
				[]() -> int { throw 7; }, 1u, error);
			context.Expect(!unknown && error.find("unknown") != std::string::npos,
				"Non-standard worker exceptions must also remain inside the precommit boundary.");
			const auto recovered = TerraEngine::Editor::Terrain::RunTerrainPrecommitJob(*pool,
				[] { return 42; }, 1u, error);
			context.Expect(recovered && *recovered == 42 && error.empty(),
				"A rejected task must not poison subsequent valid preparation on the same owned pool.");
		});
		registry.Add("Editor.TerrainPrecommit.PendingWorkerPollingDoesNotJoinOwner", [](TestContext& context) {
			// The protected owner must continue frames while an actual worker waits.
			auto pool = ThreadPool::CreateOwned(1u);
			std::promise<void> release; auto gate = release.get_future().share();
			auto task = pool->createTask([gate] { gate.wait(); return 23; });
			auto future = task->getFuture(); pool->enqueue(task, 1u);
			std::optional<int> output; std::string error;
			const auto polled = TerraEngine::Editor::Terrain::PollTerrainPrecommitJob(future, output, error);
			// Release before assertions so every failure path settles the worker.
			release.set_value();
			context.Expect(polled == TerraEngine::Editor::Terrain::TerrainPreparationPoll::Pending && !output,
				"Outstanding preparation must return Pending without joining or publishing.");
			context.Expect(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "Released worker must settle.");
			context.Expect(TerraEngine::Editor::Terrain::PollTerrainPrecommitJob(future, output, error)
				== TerraEngine::Editor::Terrain::TerrainPreparationPoll::Ready && output == 23,
				"Ready preparation crosses the boundary only when consumed by its owner.");
		});
		registry.Add("Editor.TerrainPrecommit.TimeoutDiscardsLateOwnedResult", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1u);
			auto release = std::make_shared<std::promise<void>>();
			const auto released = release->get_future().share();
			// Always unblock before draining, including when any assertion throws.
			struct ReleaseAndDrain
			{
				std::shared_ptr<std::promise<void>> release;
				ThreadPool& pool;
				bool opened = false;
				void Open() { if(!opened) { release->set_value(); opened = true; } }
				~ReleaseAndDrain() { Open(); pool.shutdown(); }
			} cleanup{release, *pool};
			auto entered = std::make_shared<std::promise<void>>(); auto started = entered->get_future();
			auto payload = std::make_shared<const std::vector<int>>(4u, 17);
			std::weak_ptr<const std::vector<int>> retained = payload;
			std::string error;
			const auto rejected = TerraEngine::Editor::Terrain::RunTerrainPrecommitJob(*pool,
				[payload, released, entered] { entered->set_value(); released.wait(); return payload->front(); },
				4u, error, std::chrono::milliseconds(10));
			context.Expect(!rejected && error.find("deadline") != std::string::npos,
				"The protected caller must return rejection while the worker is still deliberately held.");
			context.Expect(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
				"The actual held worker must start within the bounded fixture watchdog.");
			payload.reset();
			context.Expect(!retained.expired(), "Late work must retain its own immutable inputs after caller rejection.");
			cleanup.Open(); pool->shutdown();
			context.Expect(retained.expired(), "Drained late work must release its owned inputs without returning a publishable result.");
			// A separate real owner demonstrates recovery without reusing a closed pool.
			auto recoveredPool = ThreadPool::CreateOwned(1u);
			const auto recovered = TerraEngine::Editor::Terrain::RunTerrainPrecommitJob(*recoveredPool,
				[] { return 23; }, 1u, error);
			context.Expect(recovered && *recovered == 23, "A later valid precommit must still succeed.");
		});
	}
}
