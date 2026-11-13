// ExportCancellationPublicationTests.cpp - publication arbitration and observation.
#include "TestHarness.h"
#include "Editor/Export/ExportTypes.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"
#include "ThreadPool/ThreadPool.h"

#include <atomic>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <iostream>

namespace TerraEngine::EditorTests {
	namespace {
		using namespace TerraEngine::Tests::Correctness;
		struct ReleasePublicationOnExit {
			ControlledGate& gate;
			~ReleasePublicationOnExit() { gate.Release(); }
		};
		std::string ReadPublishedBytes(const std::filesystem::path& file) {
			std::ifstream input(file, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}
	}

	void RegisterExportCancellationPublicationTests(TestRegistry& registry) {
		registry.Add("Editor.Review.ExportCancellation.ObservationWhilePublicationIsHeld", [](TestContext& context) {
			DisposableFixturePath fixture;
			ControlledGate publication;
			auto state = std::make_shared<ExportCancellationState>();
			const auto ownerThread = std::this_thread::get_id();
			std::thread::id publicationThread, publicationSettledThread;
			const auto staged = fixture.Child("staged.tmp");
			const auto final = fixture.Child("published.bin");
			auto pool = ThreadPool::CreateOwned(2);
			// The IO gate deadline must exceed the observation coordination bound;
			// otherwise an automatic gate timeout could masquerade as responsive IO.
			auto commit = pool->createTask([state, wait = publication.MakeWaiter(std::chrono::seconds(30)), staged, final,
				&publicationThread, &publicationSettledThread] {
				return state->Commit([&] {
					// Observe the worker doing actual staged I/O and atomic replacement.
					publicationThread = std::this_thread::get_id();
					// Bulk preparation is deliberately held before the atomic rename.
					{ std::ofstream output(staged, std::ios::binary); output << "complete published bytes"; }
					if (wait() != GateResult::Released) return false;
					std::filesystem::rename(staged, final);
					publicationSettledThread = std::this_thread::get_id();
					return true;
				});
			});
			auto committed = commit->getFuture();
			// Release before pool destruction on every assertion/exception path.
			ReleasePublicationOnExit release{publication};
			pool->enqueue(commit);
			const bool reachedPublication = publication.WaitForArrivals(1);
			const bool invisibleBeforePublish = !std::filesystem::exists(final);
			auto observe = pool->createTask([state] {
				const bool alreadyCommitted = state->WasCommitted();
				return std::tuple{alreadyCommitted, state->RequestCancellation(), std::this_thread::get_id()};
			});
			auto observed = observe->getFuture();
			pool->enqueue(observe);
			// This is a deadlock coordination bound, not a performance threshold.
			// Readiness before releasing IO proves observation owns a separate lock.
			const bool observedWhileHeld = observed.wait_for(kDefaultTimeout) == std::future_status::ready;
			publication.Release();
			const bool commitSettled = committed.wait_for(kDefaultTimeout) == std::future_status::ready;
			const bool observationSettled = observed.wait_for(kDefaultTimeout) == std::future_status::ready;
			context.Expect(commitSettled && observationSettled, "Both real tasks must settle after publication is released.");
			const bool success = committed.get();
			const auto observation = observed.get();
			std::cout << "Export publication identities: owner=" << ownerThread << " publisher=" << publicationThread
				<< ':' << publicationSettledThread << " observer=" << std::get<2>(observation) << '\n';
			context.Expect(publicationThread != std::thread::id{} && publicationThread != ownerThread
				&& publicationSettledThread == publicationThread && std::get<2>(observation) != ownerThread
				&& std::get<2>(observation) != publicationThread && std::this_thread::get_id() == ownerThread,
				"Actual publication and responsive cancellation must run on distinct observed workers before returning to the owner.");
			context.Expect(reachedPublication && invisibleBeforePublish && observedWhileHeld,
				"Protected status/cancellation must complete while bulk publication IO remains held.");
			context.Expect(!std::get<0>(observation) && std::get<1>(observation) == ExportCancellationDisposition::TooLate,
				"Publication admission must linearize cancellation before IO while committed status stays false until completion.");
			context.Expect(success && state->WasCommitted() && ReadPublishedBytes(final) == "complete published bytes",
				"The admitted publication must expose one complete final output after its rename.");
		});

		registry.Add("Editor.Review.ExportCancellation.PrepublicationCancelPreservesOutput", [](TestContext& context) {
			DisposableFixturePath fixture;
			const auto final = fixture.Child("last-good.bin");
			{ std::ofstream output(final, std::ios::binary); output << "last good"; }
			ExportCancellationState state;
			const auto disposition = state.RequestCancellation();
			bool callbackInvoked = false;
			const bool committed = state.Commit([&] {
				callbackInvoked = true;
				std::ofstream output(final, std::ios::binary | std::ios::trunc); output << "unexpected";
				return true;
			});
			context.Expect(disposition == ExportCancellationDisposition::Accepted && !committed && !callbackInvoked,
				"A cancellation that wins admission must reject before invoking publication IO.");
			context.Expect(ReadPublishedBytes(final) == "last good", "Rejected publication must preserve the previous output bytes.");
		});
	}
}
