// RenderFrameScratchTests.cpp - Real PMR ownership and episode lifetime checks.
#include "TestHarness.h"
#include "Core/Memory/TrackingMemoryResource.h"
#include "Renderer/Graph/RenderFrameScratch.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "ThreadPool/ThreadPool.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "../../Support/RenderCaptureBackendTestAccess.h"

#include <array>
#include <atomic>

#include <future>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using Renderer::RenderFrameScratch;
		using Renderer::RenderFrameScratchOutcome;
		static_assert(!std::is_copy_constructible_v<RenderFrameScratch::Lease>);
		static_assert(std::is_nothrow_move_constructible_v<RenderFrameScratch::Lease>);
		static_assert(RenderFrameScratch::kDefaultCapacityBytes == 16U * 1024U * 1024U);

		// Native evidence is emitted only by the owning test thread, after any
		// borrower settles. All values come from actual helper/upstream observations;
		// an Active snapshot is labeled as such rather than fabricated as terminal.
		void Observe(std::string_view name, const Renderer::RenderFrameScratchDiagnostics& facts,
			std::initializer_list<std::pair<const char*, std::uint64_t>> extra = {})
		{
			std::cout << "RMS_OBSERVATION {\"case\":\"Engine.RuntimeMemorySafety.RendererScratch." << name
				<< "\",\"lifetimeId\":" << facts.lifetimeId << ",\"episodeId\":" << facts.episodeId
				<< ",\"frameId\":" << facts.frameId << ",\"capacityBytes\":" << facts.capacityBytes
				<< ",\"usedBytes\":" << facts.usedBytes << ",\"peakBytes\":" << facts.peakBytes
				<< ",\"lifetimeHighWaterBytes\":" << facts.lifetimeHighWaterBytes
				<< ",\"reuseAllocationCount\":" << facts.reuseAllocationCount
				<< ",\"fallbackAllocationCount\":" << facts.fallbackAllocationCount
				<< ",\"resetCount\":" << facts.resetCount
				<< ",\"outcome\":" << static_cast<unsigned>(facts.outcome);
			for(const auto& [key, value] : extra) std::cout << ",\"" << key << "\":" << value;
			std::cout << "}\n";
		}

		bool RejectsOverlap(const std::shared_ptr<RenderFrameScratch>& owner)
		{
			try { auto duplicate = owner->BeginEpisode(999U); }
			catch(const std::logic_error&) { return true; }
			return false;
		}
		// Declare after the async future: unwind releases the gate before that
		// future's destructor joins its borrower. The worker also has a timeout.
		struct ReleaseBorrower {
			std::promise<void>& gate;
			bool released = false;
			void Release() noexcept {
				if(released) return;
				released = true;
				try { gate.set_value(); } catch(...) {}
			}
			~ReleaseBorrower() { Release(); }
		};
		// The backend records only the real coordinator's typed calls. All capture,
		// preparation, sealing, suspension and retirement still run in RenderSystem.
		class StageBackend final : public RHIContext {
		public:
			std::size_t begins = 0U, ends = 0U;
			std::vector<std::uint32_t> viewIndices;
			std::shared_ptr<const Renderer::RenderFramePublication> publication;
			void Initialize(WindowSystem&) override {}
			void BeginFrame() override { ++begins; }
			void EndFrame() override { ++ends; }
			void WaitIdle() override {}
			void Shutdown() override { publication.reset(); }
			void RenderFrame(std::shared_ptr<const Renderer::RenderFramePublication> root,
				std::uint32_t viewIndex) override {
				if(publication && publication != root)
					throw std::logic_error("One backend frame received different publication roots");
				publication = std::move(root); viewIndices.push_back(viewIndex);
			}
			Renderer::BackendCapabilityProfile GetBackendCapabilityProfile() const override {
				auto result = RHIContext::GetBackendCapabilityProfile();
				result.maxRecordingWorkers = 2U;
				return result;
			}
		};
		// Singleton replacement is legal only at this quiescent native test boundary.
		// Destroy the continuation before authored inputs and any owned pool workers.
		struct StageFixture {
			RenderSystem& renderer = RenderSystem::Instance();
			bool ownsPool = ThreadPool::instance().getThreadCount() == 0U;
			StageBackend* backend = nullptr;
			std::vector<Object> lights;
			StageFixture() {
				bool attached = false;
				try {
					if(ownsPool) ThreadPool::instance().initialize(2U);
					renderer.Clear();
					auto context = std::make_unique<StageBackend>(); backend = context.get();
					if(!Renderer::RenderCaptureBackendTestAccess::Attach(renderer, std::move(context)))
						throw std::logic_error("Stage fixture cannot replace a retained backend");
					attached = true;
					// Child-admission assertions require parallel preparation even if a
					// preceding fixture left serial comparison options behind. Shutdown
					// resets these transient options when this fixture retires its owner.
					auto options = renderer.GetRendererEfficiencyOptionsSnapshot().options;
					options.validationPreparation = Renderer::RendererEfficiencyPreparation::Concurrent;
					if(!renderer.QueueRendererEfficiencyOptions(options).accepted)
						throw std::logic_error("Stage fixture cannot select parallel preparation");
					renderer.SetEditorModeEnabled(true);
					renderer.ClearOverrideCameras();
					renderer.SetGameViewSuppressed(false);
					renderer.SetSceneViewCamera(glm::mat4(1.0f), glm::mat4(1.0f), {});
					renderer.SetGameViewCamera(glm::mat4(1.0f), glm::mat4(1.0f), {});
					renderer.SetSceneViewTargetSize({7U, 3U});
					renderer.SetGameViewTargetSize({5U, 2U});
				} catch(...) {
					// A failed constructor has no fixture destructor to retire its owners.
					if(attached) { renderer.Shutdown(); renderer.ClearOverrideCameras(); renderer.SetEditorModeEnabled(false); }
					if(ownsPool) ThreadPool::instance().shutdown();
					throw;
				}
			}
			void AuthorLights() {
				lights.reserve(128U);
				for(std::size_t index = 0U; index < 128U; ++index) {
					const auto object = ObjectManager::instance().createObject("Stage contract light");
					lights.push_back(object);
					ObjectManager::instance().AddComponent<LightComponent>(object);
				}
			}
			~StageFixture() {
				renderer.Shutdown();
				renderer.SetFrameScratchValidationCallbacks(nullptr);
				renderer.ClearOverrideCameras();
				renderer.SetEditorModeEnabled(false);
				for(const auto object : lights) ObjectManager::instance().removeObject(object);
				if(ownsPool) ThreadPool::instance().shutdown();
			}
		};
		struct StageBoundaryObservation {
			bool active = false, rootRetained = false;
			std::size_t begins = 0U, renders = 0U, ends = 0U;
		};
		// Destruction records whether the PMR owner still had its episode lease.
		// No throwing assertion is allowed while destructors unwind a failure.
		struct TrackedValue {
			RenderFrameScratch* owner;
			bool* retiredWhileActive;
			~TrackedValue() { *retiredWhileActive = owner->IsActive(); }
		};
	}

	void RegisterRenderFrameScratchTests(TestRegistry& registry)
	{
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.ProductionStagesRetainRootsAcrossSuspensions", [](TestContext& context) {
			StageFixture fixture;
			auto& objects = ObjectManager::instance();
			objects.PublishSceneChangePacket();
			auto protectedRoot = objects.LoadPublishedSceneChangePacketShared();
			std::weak_ptr<const TerraEngine::Scene::SceneChangePacket> capturedRoot = protectedRoot;
			Renderer::RenderFrameScratchDiagnostics captured;
			auto callbacks = std::make_shared<Renderer::RenderFrameScratchValidationCallbacks>();
			callbacks->onEvent = [&](const Renderer::RenderFrameScratchValidationEvent& event) {
				if(event.checkpoint == Renderer::RenderFrameScratchValidationCheckpoint::Captured) captured = event.episode;
			};
			fixture.renderer.SetFrameScratchValidationCallbacks(callbacks);
			auto stages = fixture.renderer.DeclareFrameWorkloads(0.0f);
			const std::array<std::string_view, 5> names{"RenderPublicationCapture", "GeometryRoutePrepare",
				"VirtualGeometryPrepare", "BackendCommit", "RenderPublication"};
			context.Expect(stages.size() == names.size(), "The production coordinator must expose four resumes and final retirement");
			std::array<StageBoundaryObservation, 5> entries{}, exits{};
			const auto observe = [&] {
				return StageBoundaryObservation{fixture.renderer.GetFrameScratchValidationState().active,
					!capturedRoot.expired(), fixture.backend->begins, fixture.backend->viewIndices.size(), fixture.backend->ends};
			};
			// Wrap the actual declared callbacks; this records their observable edges,
			// rather than reproducing their coroutine or scheduler in a test model.
			for(std::size_t index = 0U; index < stages.size(); ++index) {
				context.Expect(stages[index].workloadName == names[index], "Production stage order changed");
				auto work = std::move(stages[index].work);
				stages[index].work = [&, index, work = std::move(work)](TerraEngine::Scheduling::JobGraphContext& job) {
					entries[index] = observe(); work(job); exits[index] = observe();
				};
			}
			TerraEngine::Scheduling::JobGraphContext job;
			stages[0].work(job);
			// Replacing the public packet leaves only the suspended frame holding
			// this exact old protected root. No scene generation or live input changes.
			objects.PublishSceneChangePacket(); protectedRoot.reset();
			for(std::size_t index = 1U; index < stages.size(); ++index) stages[index].work(job);
			context.Expect(captured.episodeId != 0U && !entries[0].active,
				"Capture must start a real scratch episode from a quiescent owner");
			for(std::size_t index = 0U; index < 3U; ++index) {
				context.Expect(exits[index].active && exits[index].rootRetained && exits[index].begins == 0U,
					"Every production suspension must retain its lease and exact protected root before any backend work");
				context.Expect(entries[index + 1U].active && entries[index + 1U].rootRetained,
					"The next resume must inherit the same owned inputs and active scratch episode");
			}
			context.Expect(exits[3].begins == 1U && exits[3].renders == 2U && exits[3].ends == 1U
				&& !exits[3].active && !exits[4].active && !exits[4].rootRetained,
				"Backend completion must settle owned inputs; final publication must retire without submitting more work");
			context.Expect(exits[4].begins == exits[3].begins && exits[4].renders == exits[3].renders
				&& exits[4].ends == exits[3].ends, "The fifth callback is retirement only");
			const auto publication = fixture.backend->publication;
			context.Expect(publication && publication->views.size() == 2U
				&& fixture.backend->viewIndices == std::vector<std::uint32_t>{0U, 1U}
				&& publication->views[0].outputKind == RenderOutputKind::SceneView
				&& publication->views[0].outputId == 1U
				&& publication->views[1].outputKind == RenderOutputKind::GameView
				&& publication->views[1].outputId == 2U,
				"Actual typed Scene and Game selections must reach the backend in their original output order");
			const auto terminal = fixture.renderer.GetFrameScratchValidationState();
			context.Expect(terminal.latest.episodeId == captured.episodeId
				&& terminal.latest.outcome == RenderFrameScratchOutcome::Completed,
				"Final retirement must publish the exact completed episode, not an active or replacement snapshot");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.ProductionFailureSettlesAdmittedLightChildren", [](TestContext& context) {
			StageFixture fixture; fixture.AuthorLights();
			ObjectManager::instance().PublishSceneChangePacket();
			std::promise<void> childEntered, releaseChild, inlineFailed;
			auto entered = childEntered.get_future().share();
			auto released = releaseChild.get_future().share();
			auto failed = inlineFailed.get_future();
			std::atomic<bool> selected = false, childEnded = false, childEndedWhileActive = false;
			std::atomic<std::size_t> heldStart = 0U;
			Renderer::RenderFrameScratchDiagnostics captured;
			auto callbacks = std::make_shared<Renderer::RenderFrameScratchValidationCallbacks>();
			callbacks->onEvent = [&](const Renderer::RenderFrameScratchValidationEvent& event) {
				using Checkpoint = Renderer::RenderFrameScratchValidationCheckpoint;
				if(event.checkpoint == Checkpoint::Captured) { captured = event.episode; return; }
				if(event.checkpoint == Checkpoint::LightReadBegin && event.start != 0U && !selected.exchange(true)) {
					heldStart.store(event.start); childEntered.set_value();
					if(released.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
						throw std::runtime_error("Production light child gate timed out");
				}
				if(event.checkpoint == Checkpoint::LightReadBegin && event.start == 0U) {
					if(entered.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
						throw std::runtime_error("Production light child was not admitted");
					inlineFailed.set_value();
					throw std::runtime_error("Stage contract inline failure after child admission");
				}
				if(event.checkpoint == Checkpoint::LightReadEnd && event.start == heldStart.load()) {
					childEndedWhileActive.store(fixture.renderer.GetFrameScratchValidationState().active);
					childEnded.store(true);
				}
			};
			fixture.renderer.SetFrameScratchValidationCallbacks(callbacks);
			std::weak_ptr<const Renderer::RenderFrameScratchValidationCallbacks> capturedCallbacks = callbacks;
			auto stages = fixture.renderer.DeclareFrameWorkloads(0.0f);
			TerraEngine::Scheduling::JobGraphContext job; stages.at(0).work(job);
			fixture.renderer.SetFrameScratchValidationCallbacks(nullptr); callbacks.reset();
			// Keep engine pool workers available for actual admitted children. The
			// release guard is later than the future, so failure unwinds without a join deadlock.
			auto phase = std::async(std::launch::async, [&] { stages.at(1).work(job); });
			ReleaseBorrower release{releaseChild};
			context.Expect(entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready
				&& failed.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
				"The real preparation stage must admit a child before the injected inline failure");
			context.Expect(fixture.renderer.GetFrameScratchValidationState().active && !capturedCallbacks.expired()
				&& phase.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready && !childEnded.load(),
				"Failure cannot retire its input owner or settle the stage while an admitted child still borrows lights");
			bool clearRejected = false;
			try { fixture.renderer.Clear(); } catch(const std::logic_error&) { clearRejected = true; }
			context.Expect(clearRejected && fixture.renderer.GetFrameScratchValidationState().active,
				"A competing lifecycle reset must reject atomically while production preparation owns borrowers");
			release.Release();
			std::string failure;
			try { phase.get(); } catch(const std::runtime_error& error) { failure = error.what(); }
			context.Expect(failure == "Stage contract inline failure after child admission"
				&& childEnded.load() && childEndedWhileActive.load() && capturedCallbacks.expired(),
				"Original inline failure must propagate only after the child reads and releases its captured owner");
			fixture.renderer.Clear();
			const auto terminal = fixture.renderer.GetFrameScratchValidationState();
			context.Expect(!terminal.active && terminal.latest.episodeId == captured.episodeId
				&& terminal.latest.outcome == RenderFrameScratchOutcome::Failed
				&& fixture.backend->begins == 0U && fixture.backend->viewIndices.empty(),
				"A failed production episode must retire exactly once without partial backend publication");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.ExclusiveLeaseBeforeReset", [](TestContext& context) {
			auto owner = RenderFrameScratch::Create(256U);
			auto lease = owner->BeginEpisode(1U);
			std::pmr::vector<int> values(lease.Resource());
			values.assign(16U, 77);
			const auto before = owner->Snapshot();
			const bool rejected = RejectsOverlap(owner);
			bool retireRejected = false;
			try { owner->Retire(); } catch(const std::logic_error&) { retireRejected = true; }
			const auto after = owner->Snapshot();
			Observe("ExclusiveLeaseBeforeReset", after, {{"overlapRejected", rejected}, {"activeRetireRejected", retireRejected},
				{"resetCountBeforeOverlap", before.resetCount}, {"usedBytesBeforeOverlap", before.usedBytes}});
			context.Expect(rejected && retireRejected && owner->IsActive(), "overlap and active retirement must reject before changing active storage");
			context.Expect(after.resetCount == before.resetCount && after.usedBytes == before.usedBytes && values.back() == 77,
				"a rejected episode must preserve reset count and vector contents");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.VectorOwnersRetireBeforeLease", [](TestContext& context) {
			auto owner = RenderFrameScratch::Create(1024U);
			bool retiredWhileActive = false;
			{
				auto lease = owner->BeginEpisode(2U);
				std::pmr::vector<TrackedValue> values(lease.Resource());
				values.reserve(1U);
				values.emplace_back(owner.get(), &retiredWhileActive);
				lease.MarkCompleted();
			}
			Observe("VectorOwnersRetireBeforeLease", owner->LastSnapshot(), {{"vectorsRetiredWhileActive", retiredWhileActive}, {"activeAfterRetirement", owner->IsActive()}});
			context.Expect(retiredWhileActive && !owner->IsActive(), "container elements must retire before their move-only lease releases");
			context.Expect(owner->LastSnapshot().outcome == RenderFrameScratchOutcome::Completed, "completion must publish a copied terminal outcome");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.HeldWorkerBorrower", [](TestContext& context) {
			auto owner = RenderFrameScratch::Create(1024U);
			auto lease = owner->BeginEpisode(3U);
			std::pmr::vector<int> values(lease.Resource()); values.assign(64U, 41);
			std::promise<void> entered, release;
			auto enteredFuture = entered.get_future(); auto releaseFuture = release.get_future();
			auto borrower = std::async(std::launch::async, [&] {
				entered.set_value();
				if(releaseFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return -1;
				return values.front() + values.back();
			});
			ReleaseBorrower releaseGuard{release};
			const bool enteredInTime = enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
			const bool rejected = enteredInTime && RejectsOverlap(owner);
			releaseGuard.Release();
			const bool settledInTime = borrower.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
			// Even a failed timing check settles the bounded borrower before assertions
			// can destroy the values it reads. The installed watchdog is the outer bound.
			const int observed = borrower.get();
			Observe("HeldWorkerBorrower", owner->Snapshot(), {{"borrowerEntered", enteredInTime}, {"borrowerSettled", settledInTime},
				{"overlapRejected", rejected}, {"borrowedValuesMatched", observed == 82}});
			context.Expect(enteredInTime && settledInTime && rejected && observed == 82 && owner->IsActive(),
				"a held real worker borrower must prevent reset and keep stable data");
			lease.MarkCompleted();
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.FailureAfterBorrowerSettlement", [](TestContext& context) {
			auto owner = RenderFrameScratch::Create(256U);
			bool retiredWhileActive = false;
			try {
				auto lease = owner->BeginEpisode(4U);
				std::pmr::vector<TrackedValue> values(lease.Resource()); values.reserve(1U); values.emplace_back(owner.get(), &retiredWhileActive);
				auto borrower = std::async(std::launch::async, [&] { return values.size(); });
				(void)borrower.get();
				throw std::runtime_error("fixture preparation failure");
			} catch(const std::runtime_error&) {}
			const auto failed = owner->LastSnapshot();
			Observe("FailureAfterBorrowerSettlement", failed, {{"vectorsRetiredWhileActive", retiredWhileActive},
				{"activeAfterFailure", owner->IsActive()}});
			context.Expect(retiredWhileActive && !owner->IsActive() && failed.outcome == RenderFrameScratchOutcome::Failed,
				"unwinding after child settlement must retire vectors, mark failed, and release without worker reset");
			const auto resets = failed.resetCount; owner->Retire();
			context.Expect(owner->LastSnapshot().resetCount == resets, "protected cleanup cannot rewrite the pre-reset failure evidence");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.CancelledEpisodeRetirement", [](TestContext& context) {
			auto owner = RenderFrameScratch::Create(128U);
			{
				auto lease = owner->BeginEpisode(5U);
				std::pmr::vector<int> values(lease.Resource()); values.push_back(5);
				// Destroying a suspended continuation without successful completion
				// cancels it; the next protected episode owns actual reclamation.
			}
			const auto cancelled = owner->LastSnapshot();
			context.Expect(cancelled.outcome == RenderFrameScratchOutcome::Cancelled && cancelled.usedBytes > 0U && !owner->IsActive(),
				"abandoning an episode must retain cancelled storage-use evidence");
			auto next = owner->BeginEpisode(6U);
			Observe("CancelledEpisodeRetirement", cancelled, {{"nextEpisodeId", owner->Snapshot().episodeId},
				{"resetCountAfterProtectedBegin", owner->Snapshot().resetCount}});
			context.Expect(owner->Snapshot().resetCount == cancelled.resetCount + 1U, "next protected capture must reclaim the cancelled episode");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.SceneClearOwnerRetirement", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(8U, &upstream);
			std::weak_ptr<RenderFrameScratch> previous = owner;
			Renderer::RenderFrameScratchDiagnostics beforeRetirement;
			{
				auto lease = owner->BeginEpisode(7U);
				std::pmr::vector<int> values(lease.Resource()); values.assign(128U, 13);
				beforeRetirement = owner->Snapshot();
				owner.reset();
				context.Expect(!previous.expired() && values.back() == 13, "retiring a scene owner cannot invalidate a retained continuation lease");
			}
			Observe("SceneClearOwnerRetirement", beforeRetirement, {{"snapshotBeforeOwnerRetirement", 1U},
				{"ownerExpiredAfterRetirement", previous.expired()}, {"upstreamAllocations", upstream.Statistics().allocationCount},
				{"upstreamDeallocations", upstream.Statistics().deallocationCount}, {"upstreamBytesAfterCleanup", upstream.Statistics().bytesInUse}});
			context.Expect(previous.expired() && upstream.Statistics().bytesInUse == 0U, "retired lifetime cleanup must balance all fallback ownership");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.ShutdownStrongOwnership", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(0U, &upstream);
			std::weak_ptr<RenderFrameScratch> weak = owner;
			Renderer::RenderFrameScratchDiagnostics beforeShutdown;
			{
				auto original = owner->BeginEpisode(8U);
				auto lease = std::move(original);
				std::pmr::vector<int> values(lease.Resource()); values.assign(8U, 19);
				beforeShutdown = owner->Snapshot();
				owner.reset();
				context.Expect(!weak.expired() && values.front() == 19, "a moved lease must retain the same resource across external shutdown ownership release");
			}
			context.Expect(weak.expired() && upstream.Statistics().allocationCount == upstream.Statistics().deallocationCount,
				"last lease retirement must release every fallback even after the lifecycle owner disappears");
			// Move assignment must release the destination's old episode before
			// taking another lifetime; both resources remain independently owned.
			auto firstOwner = RenderFrameScratch::Create(0U, &upstream);
			auto secondOwner = RenderFrameScratch::Create(32U);
			std::weak_ptr<RenderFrameScratch> firstWeak = firstOwner;
			auto destination = firstOwner->BeginEpisode(81U);
			{
				std::pmr::vector<int> temporary(destination.Resource()); temporary.assign(8U, 23);
			}
			auto source = secondOwner->BeginEpisode(82U);
			firstOwner.reset();
			destination = std::move(source);
			Observe("ShutdownStrongOwnership", beforeShutdown, {{"snapshotBeforeOwnerRetirement", 1U},
				{"shutdownOwnerExpired", weak.expired()}, {"moveAssignmentPriorOwnerExpired", firstWeak.expired()},
				{"incomingEpisodeActive", secondOwner->IsActive()}, {"upstreamAllocations", upstream.Statistics().allocationCount},
				{"upstreamDeallocations", upstream.Statistics().deallocationCount}, {"upstreamBytesAfterCleanup", upstream.Statistics().bytesInUse}});
			context.Expect(firstWeak.expired() && upstream.Statistics().bytesInUse == 0U && secondOwner->IsActive(),
				"move assignment must release its prior owner without ending the incoming episode");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.RestartLifetimeIdentity", [](TestContext& context) {
			auto previous = RenderFrameScratch::Create(32U);
			auto replacement = RenderFrameScratch::Create(32U);
			auto oldLease = previous->BeginEpisode(1U); auto newLease = replacement->BeginEpisode(1U);
			const auto oldFacts = previous->Snapshot(); const auto newFacts = replacement->Snapshot();
			Observe("RestartLifetimeIdentity", newFacts, {{"priorLifetimeId", oldFacts.lifetimeId}, {"priorEpisodeId", oldFacts.episodeId}});
			context.Expect(oldFacts.lifetimeId != 0U && newFacts.lifetimeId != 0U && oldFacts.lifetimeId != newFacts.lifetimeId,
				"reinitialization must distinguish renderer lifetimes even when frame IDs repeat");
			context.Expect(oldFacts.episodeId == 1U && newFacts.episodeId == 1U, "new renderer owners begin independent episode sequences");
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.FallbackBalancedAtProtectedReset", [](TestContext& context) {
			Memory::TrackingMemoryResource upstream;
			auto owner = RenderFrameScratch::Create(8U, &upstream);
			{
				auto lease = owner->BeginEpisode(9U);
				std::pmr::vector<int> values(lease.Resource()); values.assign(128U, 9);
				lease.MarkCompleted();
			}
			const auto last = owner->LastSnapshot();
			const auto retainedFallbackBytes = upstream.Statistics().bytesInUse;
			const auto beforeReclamation = owner->ReclamationSnapshot();
			context.Expect(last.fallbackAllocationCount > 0U && upstream.Statistics().bytesInUse > 0U,
				"release must expose real fallback pressure and leave reclamation to protected reset");
			context.Expect(beforeReclamation.lifetimeId == last.lifetimeId
				&& beforeReclamation.totalFallbackAllocationCount == upstream.Statistics().allocationCount
				&& beforeReclamation.totalFallbackDeallocationCount == upstream.Statistics().deallocationCount
				&& beforeReclamation.outstandingFallbackAllocationCount == 1U
				&& beforeReclamation.outstandingFallbackBytes == retainedFallbackBytes
				&& beforeReclamation.resetCount == last.resetCount,
				"quiescent lifetime snapshot must report actual retained allocation and upstream counts");
			{
				auto next = owner->BeginEpisode(10U);
				const auto afterProtectedBegin = owner->ReclamationSnapshot();
				context.Expect(upstream.Statistics().bytesInUse == 0U && owner->Snapshot().fallbackAllocationCount == 0U,
					"protected next capture must balance prior fallback and begin fresh episode deltas");
				context.Expect(afterProtectedBegin.totalFallbackAllocationCount == upstream.Statistics().allocationCount
					&& afterProtectedBegin.totalFallbackDeallocationCount == upstream.Statistics().deallocationCount
					&& afterProtectedBegin.outstandingFallbackAllocationCount == 0U
					&& afterProtectedBegin.outstandingFallbackBytes == 0U
					&& afterProtectedBegin.resetCount == beforeReclamation.resetCount + 1U,
					"protected reset must report actual upstream deallocation and zero outstanding ownership");
				context.Expect(owner->LastSnapshot().frameId == last.frameId
					&& owner->LastSnapshot().fallbackAllocationCount == last.fallbackAllocationCount
					&& owner->LastSnapshot().resetCount == last.resetCount,
					"reclamation snapshot must not overwrite terminal pre-reset pressure fields");
			}
			owner->Retire();
			owner->Retire();
			const auto afterRepeatedRetire = owner->ReclamationSnapshot();
			context.Expect(afterRepeatedRetire.totalFallbackDeallocationCount == upstream.Statistics().deallocationCount
				&& afterRepeatedRetire.totalFallbackDeallocationCount == beforeReclamation.totalFallbackAllocationCount
				&& afterRepeatedRetire.resetCount > afterRepeatedRetire.totalFallbackDeallocationCount,
				"empty repeated retire must increase reset count without inventing deallocation events");
			{
				auto next = owner->BeginEpisode(11U);
				std::pmr::vector<int> first(next.Resource()); first.assign(128U, 11);
				std::pmr::vector<int> second(next.Resource()); second.assign(256U, 12);
				next.MarkCompleted();
			}
			const auto beforeSecondRetire = owner->ReclamationSnapshot();
			context.Expect(beforeSecondRetire.totalFallbackAllocationCount == upstream.Statistics().allocationCount
				&& beforeSecondRetire.totalFallbackDeallocationCount == upstream.Statistics().deallocationCount
				&& beforeSecondRetire.outstandingFallbackAllocationCount == 2U
				&& beforeSecondRetire.outstandingFallbackBytes == upstream.Statistics().bytesInUse,
				"second episode must retain exactly its two real upstream blocks");
			const auto terminal = owner->LastSnapshot();
			owner->Retire();
			const auto reclaimed = owner->ReclamationSnapshot();
			context.Expect(reclaimed.totalFallbackAllocationCount == upstream.Statistics().allocationCount
				&& reclaimed.totalFallbackDeallocationCount == upstream.Statistics().deallocationCount
				&& reclaimed.totalFallbackAllocationCount == reclaimed.totalFallbackDeallocationCount
				&& reclaimed.outstandingFallbackAllocationCount == 0U && reclaimed.outstandingFallbackBytes == 0U
				&& owner->LastSnapshot().frameId == terminal.frameId
				&& owner->LastSnapshot().resetCount == terminal.resetCount,
				"actual reclamation must balance upstream ownership while preserving the terminal episode copy");
			Observe("FallbackBalancedAtProtectedReset", last, {{"bytesRetainedBeforeProtectedReset", retainedFallbackBytes},
				{"upstreamAllocations", upstream.Statistics().allocationCount}, {"upstreamDeallocations", upstream.Statistics().deallocationCount},
				{"upstreamBytesAfterCleanup", upstream.Statistics().bytesInUse},
				{"totalFallbackAllocationCount", reclaimed.totalFallbackAllocationCount},
				{"totalFallbackDeallocationCount", reclaimed.totalFallbackDeallocationCount},
				{"outstandingFallbackAllocationCount", reclaimed.outstandingFallbackAllocationCount},
				{"outstandingFallbackBytes", reclaimed.outstandingFallbackBytes},
				{"reclamationResetCount", reclaimed.resetCount}});
		});
		registry.Add("Engine.RuntimeMemorySafety.RendererScratch.CopiedDiagnosticsAcrossReset", [](TestContext& context) {
			auto owner = RenderFrameScratch::Create(1024U);
			{
				auto lease = owner->BeginEpisode(11U);
				std::pmr::vector<int> values(lease.Resource()); values.assign(32U, 17);
				lease.MarkCompleted();
			}
			const auto copied = owner->LastSnapshot();
			{
				auto next = owner->BeginEpisode(12U);
				const auto current = owner->Snapshot();
				Observe("CopiedDiagnosticsAcrossReset", copied, {{"nextEpisodeId", current.episodeId}, {"nextUsedBytes", current.usedBytes},
					{"nextResetCount", current.resetCount}, {"nextLifetimeHighWaterBytes", current.lifetimeHighWaterBytes},
					{"preservedTerminalFrameId", owner->LastSnapshot().frameId}});
				context.Expect(copied.frameId == 11U && copied.episodeId == 1U && copied.usedBytes >= 32U * sizeof(int)
					&& copied.peakBytes == copied.usedBytes && copied.reuseAllocationCount > 0U,
					"copied diagnostics must preserve the completed episode's actual use and identities");
				context.Expect(current.frameId == 12U && current.episodeId == 2U && current.usedBytes == 0U
					&& current.lifetimeHighWaterBytes >= copied.peakBytes && current.resetCount == copied.resetCount + 1U,
					"reset must clear episode use while retaining lifetime high-water and independent copied facts");
				context.Expect(owner->LastSnapshot().frameId == copied.frameId, "opening the next episode must not overwrite prior terminal evidence");
			}
		});
	}
}
