#include "TestHarness.h"
#include "Renderer/SyntheticCapture.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "Renderer/RenderSystem.h"
#include "ThreadPool/ThreadPool.h"
#include "../../Support/DecodedCaptureAssertions.h"
#include "../../Support/RenderCaptureBackendTestAccess.h"
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>
#include <future>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests {
namespace {
	using namespace TerraEngine::Renderer;
	RenderCaptureRequest Request(std::string id = "synthetic-lifecycle")
	{
		RenderCaptureRequest request;
		request.requestId = std::move(id);
		request.sourceKind = RenderCaptureSourceKind::CustomCamera;
		request.sourceName = "Owned native capture input";
		request.targetSize = {7, 3};
		request.timeoutMs = 30000;
		return request;
	}
	// Always release test gates during assertion unwinding, before any job or
	// renderer destructor joins worker ownership. No production callback exists.
	struct ReleaseGate {
		std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint;
		~ReleaseGate() { Release(); }
		void Release() {
			checkpoint->released.store(true, std::memory_order_release); checkpoint->released.notify_all();
			checkpoint->retirementReleased.store(true, std::memory_order_release); checkpoint->retirementReleased.notify_all();
		}
	};
	template<class Predicate> bool Wait(Predicate predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!predicate()) {
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}
	struct RendererFixture {
		bool ownsPool = ThreadPool::instance().getThreadCount() == 0;
		RendererFixture() { if (ownsPool) ThreadPool::instance().initialize(2); RenderSystem::Instance().Clear(); }
		~RendererFixture() { RenderSystem::Instance().Clear(); if (ownsPool) ThreadPool::instance().shutdown(); }
	};
	struct FakeReadbackState
	{
		bool ownershipSupported = true;
		bool retained = true;
		bool emitResult = false;
		bool delivered = false;
		bool clearReleases = false;
		bool shutdownAllowed = true;
		bool shutdownComplete = false;
		unsigned admissions = 0;
		unsigned shutdowns = 0;
		unsigned clears = 0;
		unsigned destroyed = 0;
		std::string admissionId;
		RenderCaptureRequest request;
	};
	// These observations exercise RenderSystem's actual integration state. They
	// are policy fakes, explicitly separate from installed real Vulkan evidence.
	struct FakeReadbackBackend final : RHIContext
	{
		std::shared_ptr<FakeReadbackState> state;
		explicit FakeReadbackBackend(std::shared_ptr<FakeReadbackState> value) : state(std::move(value)) {}
		~FakeReadbackBackend() override { ++state->destroyed; }
		void Initialize(WindowSystem&) override {}
		void BeginFrame() override {}
		void RenderFrame(std::shared_ptr<const RenderFramePublication>, std::uint32_t) override {}
		void EndFrame() override {}
		void WaitIdle() override {}
		BackendCapabilityProfile GetBackendCapabilityProfile() const override { auto profile = RHIContext::GetBackendCapabilityProfile(); profile.supportsImageReadback = true; return profile; }
		RenderCaptureResult ReadbackCapture(const RenderCaptureRequest& request) override
		{
			++state->admissions; state->request = request; state->admissionId = request.requestId; state->delivered = false;
			RenderCaptureResult result; result.requestId = request.requestId; result.pendingAsync = true; result.errorCategory.clear();
			return result;
		}
		std::vector<RenderCaptureResult> PollCaptureReadbacks() override
		{
			if (!state->emitResult || state->delivered || state->admissionId.empty()) return {};
			RenderCaptureResult result; result.requestId = state->admissionId; result.sourceKind = state->request.sourceKind;
			result.sourceName = state->request.sourceName; result.size = state->request.targetSize;
			result.errorCategory = "fake_completion_failure"; result.message = "Counted fake backend terminal outcome.";
			state->delivered = true;
			return {std::move(result)};
		}
		CaptureReadbackOwnership GetCaptureReadbackOwnership(std::string_view id) const override
		{
			if (!state->ownershipSupported) return CaptureReadbackOwnership::Unsupported;
			return id == state->admissionId && state->retained ? CaptureReadbackOwnership::Retained : CaptureReadbackOwnership::Released;
		}
		void ClearResources() override { ++state->clears; if (state->clearReleases) state->retained = false; }
		void Shutdown() override { ++state->shutdowns; if (state->shutdownAllowed) { state->retained = false; state->shutdownComplete = true; } }
		bool IsShutdownComplete() const override { return state->shutdownComplete; }
	};
	struct FakeCleanup
	{
		std::shared_ptr<FakeReadbackState> state;
		~FakeCleanup() { state->shutdownAllowed = true; state->retained = false; RenderSystem::Instance().Shutdown(); }
	};
	RenderCaptureRequest BackendRequest(std::string id)
	{
		auto request = Request(std::move(id));
		// A published Game View is enough for a counted fake readback, avoiding
		// any dedicated render/update call and any claim of actual GPU pixels.
		request.sourceKind = RenderCaptureSourceKind::GameView;
		return request;
	}
	void PumpSettled(TestContext& context)
	{
		context.Expect(Wait([] { auto& renderer = RenderSystem::Instance(); renderer.ProcessQueuedCaptureRequests(); return renderer.GetCaptureQueueState().activeCaptureCount == 0; }), "All physically settled work must release capacity.");
	}
	// Recovery is observed through the production facade and the independent
	// decoder, after the failed owner's physical capacity has been released.
	void CheckHealthyAfterFailure(TestContext& context, RenderSystem& renderer, std::string id)
	{
		const auto request = Request(std::move(id));
		const auto result = renderer.CaptureImage(request);
		context.Expect(result.ok && result.requestId == request.requestId && result.deterministicFallback
			&& !result.backendReadback && !result.backendTarget, "Settled failure must allow an honest healthy capture with exact identity.");
		const auto decoded = TestSupport::DecodeCaptureBase64(result.base64Data);
		context.Expect(decoded.width == 7U && decoded.height == 3U && decoded.rgba.size() == 7U * 3U * 4U,
			"Healthy recovery must independently decode its complete requested image.");
		for(std::size_t offset = 0; offset < decoded.rgba.size(); offset += 4U)
			context.Expect(decoded.rgba[offset] == 0U && decoded.rgba[offset + 1U] == 0U
				&& decoded.rgba[offset + 2U] == 0U && decoded.rgba[offset + 3U] == 255U,
				"Every healthy recovery pixel must retain opaque black synthetic output.");
		PumpSettled(context);
		context.Expect(!renderer.PollCaptureResult(request.requestId), "Healthy synchronous recovery must not publish another result.");
	}
}
void RegisterSyntheticCaptureLifecycleTests(TestRegistry& registry)
{
	registry.Add("Renderer.SyntheticCapture.OwnedInputsSurviveCallerMutation", [](TestContext& context) {
		auto pool = ThreadPool::CreateOwned(1);
		SyntheticCaptureJob job;
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true;
		ReleaseGate release{checkpoint};
		auto request = Request(); std::string error;
		context.Expect(job.Start(request, 42, pool.get(), checkpoint, error), "Initialized executor must admit owned work.");
		context.Expect(Wait([&] { return checkpoint->entered.load(); }), "Held work must actually enter a worker.");
		request.sourceName = "Mutated caller"; request.targetSize = {1,1};
		release.Release();
		std::optional<RenderCaptureResult> result;
		context.Expect(Wait([&] { result = job.TakeResult(); return result.has_value(); }), "Owned work must complete.");
		job.Join();
		const auto decoded = TestSupport::DecodeCaptureBase64(result->base64Data);
		context.Expect(decoded.width == 7 && decoded.height == 3 && !result->backendTarget && result->sourceName == "Owned native capture input" && result->frameId == 42,
			"Output must come exclusively from copied request/context ownership.");
		context.Expect(checkpoint->retired.load() && job.IsPhysicallySettled() && !job.TakeResult(), "Owner destruction and result consumption must occur once.");
	});
	registry.Add("Renderer.SyntheticCapture.MissingExecutorRejectsWithoutOrphan", [](TestContext& context) {
		SyntheticCaptureJob job; std::string error;
		context.Expect(!job.Start(Request(), 0, nullptr, {}, error) && !error.empty() && job.IsPhysicallySettled() && !job.TakeResult(), "Missing executor must not create a never-run promise or physical charge.");
		job.Join();
	});
	for (const auto fault : {SyntheticCaptureFault::Admission, SyntheticCaptureFault::Allocation, SyntheticCaptureFault::Encoding, SyntheticCaptureFault::LateEncoding}) {
		registry.Add("Renderer.SyntheticCapture.Fault" + std::to_string(static_cast<int>(fault)) + "ThenHealthy", [fault](TestContext& context) {
			RendererFixture fixture; auto& renderer = RenderSystem::Instance();
			if (fault == SyntheticCaptureFault::LateEncoding) {
				const auto rolledBack = PrepareSyntheticCapture(Request(), 0, fault);
				context.Expect(!rolledBack.ok && rolledBack.base64Data.empty() && !rolledBack.deterministicFallback
					&& rolledBack.errorCategory == "capture_encoding_failed", "Failure after Base64 population must roll back helper success state and bytes.");
			}
			auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->fault = fault;
			SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
			const auto failed = renderer.CaptureImage(Request("fault"));
			context.Expect(!failed.ok && failed.base64Data.empty() && !failed.errorCategory.empty(), "Admission/allocation/encoding faults must have an empty terminal failure.");
			PumpSettled(context);
			context.Expect(!renderer.PollCaptureResult("fault"), "Consumed fault must not be redelivered.");
			context.Expect(renderer.CaptureImage(Request("healthy")).ok, "Settled failure must permit later healthy work.");
		});
	}
	registry.Add("Renderer.SyntheticCapture.TimeoutRetainsBoundedPhysicalSlotAndSuppressesLateResult", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true;
		ReleaseGate release{checkpoint}; SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		auto request = Request("held-timeout"); request.timeoutMs = 10;
		const auto timedOut = renderer.CaptureImage(request);
		context.Expect(!timedOut.ok && timedOut.errorCategory == "timeout" && timedOut.base64Data.empty(), "Held worker must publish one empty deadline failure.");
		context.ExpectEqual(1u, renderer.GetCaptureQueueState().activeCaptureCount, "Client timeout must retain physical concurrency charge.");
		std::string error;
		const auto limits = renderer.GetCaptureLimits();
		for (unsigned index = 0; index < limits.maxQueuedCaptures; ++index) context.Expect(!renderer.QueueCaptureRequest(Request("bounded-" + std::to_string(index)), error).empty(), "Pending capacity should remain bounded and usable.");
		context.Expect(renderer.QueueCaptureRequest(Request("overflow"), error).empty(), "Client-terminal running work must not create detached unbounded admission.");
		release.Release(); PumpSettled(context);
		context.Expect(!renderer.PollCaptureResult("held-timeout"), "Late success must only retire ownership, never publish a second outcome.");
		context.Expect(renderer.CaptureImage(Request("healthy-after-timeout")).ok, "Healthy admission must progress after held work settles.");
	});
	registry.Add("Renderer.SyntheticCapture.ReadyPromiseDoesNotReleasePhysicalCharge", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true; checkpoint->holdBeforeRetirement = true;
		ReleaseGate release{checkpoint}; SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		std::string error; renderer.QueueCaptureRequest(Request("promise-before-retire"), error); renderer.ProcessQueuedCaptureRequests();
		context.Expect(Wait([&] { return checkpoint->entered.load(); }), "Worker must own all captures before initial release.");
		checkpoint->released.store(true); checkpoint->released.notify_all();
		context.Expect(Wait([&] { return checkpoint->retirementEntered.load(); }), "Native checkpoint must observe callable destruction after promise readiness.");
		renderer.ProcessQueuedCaptureRequests();
		const auto result = renderer.PollCaptureResult("promise-before-retire");
		context.Expect(result && result->ok, "Prepared image can become client-visible before final capture destruction.");
		context.ExpectEqual(1u, renderer.GetCaptureQueueState().activeCaptureCount, "Ready future is not physical settlement evidence.");
		release.Release(); PumpSettled(context);
		context.Expect(!renderer.PollCaptureResult("promise-before-retire"), "Retirement cannot redeliver the prepared image.");
	});
	for (bool shutdown : {false, true}) {
		registry.Add(std::string("Renderer.SyntheticCapture.") + (shutdown ? "Shutdown" : "Clear") + "JoinsOutsideDiagnosticLocks", [shutdown](TestContext& context) {
			RendererFixture fixture; auto& renderer = RenderSystem::Instance();
			auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true;
			ReleaseGate release{checkpoint}; SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
			std::string error; renderer.QueueCaptureRequest(Request("drain-held"), error); renderer.ProcessQueuedCaptureRequests();
			context.Expect(Wait([&] { return checkpoint->entered.load(); }), "Clear fixture must hold a real admitted worker.");
			// The releasing thread observes an actual retirement episode through the
			// diagnostics lock, while the held CPU physical charge remains live.
			std::atomic<bool> observedDetached{false};
			std::jthread releaser([&] {
				observedDetached.store(Wait([&] { return SyntheticCaptureTestAccess::IsRetiringCaptures(renderer); }));
				release.Release();
			});
			if (shutdown) renderer.Shutdown(); else renderer.Clear();
			context.Expect(observedDetached.load() && checkpoint->retired.load() && renderer.GetCaptureQueueState().activeCaptureCount == 0, "Lifecycle teardown must physically join admitted ownership.");
			context.Expect(!renderer.PollCaptureResult("drain-held"), "Revoked lifetime cannot publish an old result.");
		});
	}
	registry.Add("Renderer.SyntheticCapture.ExecutorShutdownDrainsAcceptedQueuedPromise", [](TestContext& context) {
		auto pool = ThreadPool::CreateOwned(1);
		auto gate = std::make_shared<SyntheticCaptureCheckpoint>();
		ReleaseGate release{gate};
		auto blocker = pool->createTask([gate] { gate->entered.store(true); while (!gate->released.load()) gate->released.wait(false); });
		auto blocked = blocker->getFuture(); pool->enqueue(blocker); blocker.reset();
		context.Expect(Wait([&] { return gate->entered.load(); }), "First task must hold the sole worker.");
		SyntheticCaptureJob job; std::string error;
		context.Expect(job.Start(Request(), 0, pool.get(), {}, error), "Capture must be accepted behind held work.");
		release.Release(); pool->shutdown();
		job.Join(); const auto result = job.TakeResult();
		// Join consumes unobserved result during teardown; the important contract
		// is no unready self-retained promise and complete physical destruction.
		context.Expect(job.IsPhysicallySettled() && !result, "Shutdown must drain accepted work before executor destruction.");
	});
	registry.Add("Renderer.SyntheticCapture.DuplicatePublicIdsRetainExactAdmissionIdentity", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true;
		ReleaseGate release{checkpoint}; SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		std::string error; renderer.QueueCaptureRequest(Request("same-public-id"), error); renderer.ProcessQueuedCaptureRequests();
		context.Expect(Wait([&] { return checkpoint->entered.load(); }), "First admission must remain physically active.");
		auto second = Request("same-public-id"); second.timeoutMs = 5;
		context.Expect(renderer.CaptureImage(second).errorCategory == "timeout", "Second synchronous admission must time out independently of first.");
		release.Release(); PumpSettled(context);
		const auto first = renderer.PollCaptureResult("same-public-id");
		context.Expect(first && first->ok && first->requestId == "same-public-id", "Old success belongs to first public admission and cannot be lost or misassociated.");
		context.Expect(!renderer.PollCaptureResult("same-public-id"), "Both admissions must produce at most one public terminal result each.");
	});
	registry.Add("Renderer.SyntheticCapture.QueuedAsyncDeadlineRetainsWorkerAndDeliversOnce", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true;
		ReleaseGate release{checkpoint}; SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		auto request = Request("async-deadline"); request.timeoutMs = 50;
		std::string error; context.Expect(!renderer.QueueCaptureRequest(request, error).empty(), "Async admission must succeed.");
		renderer.ProcessQueuedCaptureRequests();
		context.Expect(Wait([&] { return checkpoint->entered.load(); }), "Real worker must hold async preparation.");
		std::optional<RenderCaptureResult> result;
		context.Expect(Wait([&] { renderer.ProcessQueuedCaptureRequests(); result = renderer.PollCaptureResult("async-deadline"); return result.has_value(); }), "Protected polling must expire queued async requests without CaptureImage.");
		context.Expect(!result->ok && result->errorCategory == "timeout" && result->base64Data.empty(), "Async deadline must publish one empty failure.");
		context.ExpectEqual(1u, renderer.GetCaptureQueueState().activeCaptureCount, "Async client deadline must retain running worker's slot.");
		for (unsigned i = 0; i < 8; ++i) { renderer.ProcessQueuedCaptureRequests(); context.Expect(!renderer.PollCaptureResult("async-deadline"), "Repeated deadline polls cannot redeliver."); }
		release.Release(); PumpSettled(context);
		context.Expect(!renderer.PollCaptureResult("async-deadline") && renderer.CaptureImage(Request("healthy-after-async-deadline")).ok, "Late physical completion must retire silently and allow healthy work.");
	});
	registry.Add("Renderer.SyntheticCapture.DeliveryReservationRejectsBeforeAdmission", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->fault = SyntheticCaptureFault::DeliveryReservation;
		SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		const auto rejected = renderer.CaptureImage(Request("no-delivery-storage"));
		const auto state = renderer.GetCaptureQueueState();
		context.Expect(!rejected.ok && rejected.base64Data.empty() && !checkpoint->entered.load()
			&& state.pendingCaptureCount == 0 && state.activeCaptureCount == 0 && state.completedResultCount == 0,
			"Delivery reservation failure must precede queue insertion, dispatch and terminal ownership.");
		context.Expect(!renderer.PollCaptureResult("no-delivery-storage") && renderer.CaptureImage(Request("healthy-after-reservation")).ok,
			"Rejected admission must not leave a second terminal result or wedge healthy work.");
	});
	registry.Add("Renderer.SyntheticCapture.RejectedPreparedTaskCannotArmRetirementHold", [](TestContext& context) {
		auto pool = ThreadPool::CreateOwned(1); SyntheticCaptureJob job;
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>();
		checkpoint->fault = SyntheticCaptureFault::Enqueue;
		checkpoint->holdBeforeEncoding = true; checkpoint->holdBeforeRetirement = true;
		ReleaseGate release{checkpoint}; std::string error;
		context.Expect(!job.Start(Request(), 0, pool.get(), checkpoint, error), "Typed seam rejects after task/future preparation but before enqueue acceptance.");
		context.Expect(!checkpoint->entered.load() && checkpoint->retired.load() && job.IsPhysicallySettled(),
			"Rejected never-run task must destroy all prepared captures without waiting on worker-only holds.");
		job.Join();
		SyntheticCaptureJob healthy;
		context.Expect(healthy.Start(Request(), 0, pool.get(), {}, error), "Prepared-task rejection must leave executor usable.");
		std::optional<RenderCaptureResult> result;
		context.Expect(Wait([&] { result = healthy.TakeResult(); return result.has_value(); }) && result->ok, "Healthy work must finish after rejection.");
		healthy.Join();
	});
	registry.Add("Renderer.SyntheticCapture.FakeReadbackUnsupportedRejectsBeforeAdmission", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto state = std::make_shared<FakeReadbackState>(); state->ownershipSupported = false; FakeCleanup cleanup{state};
		context.Expect(RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(state)), "Fresh fake backend must attach through the release gate.");
		auto request = BackendRequest("unsupported-owner"); request.allowBackendFallback = false;
		const auto denied = renderer.CaptureImage(request);
		context.Expect(!denied.ok && denied.errorCategory == "backend_readback_unavailable" && state->admissions == 0,
			"Unsupported ownership must refuse native admission before ReadbackCapture can own resources.");
		request.allowBackendFallback = true; request.requestId = "unsupported-fallback";
		const auto fallback = renderer.CaptureImage(request);
		context.Expect(fallback.ok && fallback.deterministicFallback && !fallback.backendTarget && state->admissions == 0,
			"Permitted fallback remains honest and owns no fake/native target provenance.");
		PumpSettled(context);
	});
	registry.Add("Renderer.SyntheticCapture.FakeReadbackTimeoutRetainsUntilSameOwnerRelease", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto state = std::make_shared<FakeReadbackState>(); FakeCleanup cleanup{state};
		context.Expect(RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(state)), "Fake backend must attach.");
		auto request = BackendRequest("native-timeout"); request.timeoutMs = 10;
		const auto timeout = renderer.CaptureImage(request);
		context.Expect(timeout.errorCategory == "timeout" && state->admissions == 1 && state->admissionId != request.requestId
			&& renderer.GetCaptureQueueState().activeCaptureCount == 1, "Client timeout must preserve exact internal native admission charge.");
		state->emitResult = true;
		for (unsigned i = 0; i < 4; ++i) renderer.ProcessQueuedCaptureRequests();
		context.Expect(!renderer.PollCaptureResult(request.requestId) && renderer.GetCaptureQueueState().activeCaptureCount == 1,
			"A late terminal backend result is neither another client response nor physical retirement proof.");
		state->retained = false; renderer.ProcessQueuedCaptureRequests(); renderer.ProcessQueuedCaptureRequests();
		context.Expect(renderer.GetCaptureQueueState().activeCaptureCount == 0 && !renderer.PollCaptureResult(request.requestId),
			"Same-owner Released must release charge once without redelivery.");
		renderer.Shutdown();
		context.Expect(renderer.CaptureImage(Request("healthy-after-native-retirement")).ok, "Healthy synthetic work must progress after native owner retires.");
	});
	registry.Add("Renderer.SyntheticCapture.FakeReadbackReleaseBeforeClientDeliveryRetainsLogicalOwner", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto state = std::make_shared<FakeReadbackState>(); FakeCleanup cleanup{state};
		context.Expect(RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(state)), "Fake backend must attach.");
		std::string error; renderer.QueueCaptureRequest(BackendRequest("released-before-delivery"), error); renderer.ProcessQueuedCaptureRequests();
		state->retained = false; renderer.ProcessQueuedCaptureRequests();
		context.Expect(renderer.GetCaptureQueueState().activeCaptureCount == 0 && !renderer.PollCaptureResult("released-before-delivery"),
			"Physical release may free capacity while preserving undelivered logical correlation.");
		state->emitResult = true; renderer.ProcessQueuedCaptureRequests();
		const auto result = renderer.PollCaptureResult("released-before-delivery");
		context.Expect(result && result->errorCategory == "fake_completion_failure" && result->requestId == "released-before-delivery",
			"Later delivery must still find the exact physically retired logical admission.");
		renderer.ProcessQueuedCaptureRequests();
		context.Expect(!renderer.PollCaptureResult("released-before-delivery") && renderer.GetCaptureQueueState().activeCaptureCount == 0,
			"Repeated physical and terminal observations must not underflow or redeliver.");
	});
	registry.Add("Renderer.SyntheticCapture.FakeReadbackClearPreservesPhysicalSessionLedger", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto state = std::make_shared<FakeReadbackState>(); FakeCleanup cleanup{state};
		context.Expect(RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(state)), "Fake backend must attach.");
		std::string error; renderer.QueueCaptureRequest(BackendRequest("clear-native-session"), error); renderer.ProcessQueuedCaptureRequests();
		const auto oldAdmission = state->admissionId;
		renderer.Clear();
		context.Expect(state->clears == 1 && renderer.GetCaptureQueueState().activeCaptureCount == 1 && !renderer.PollCaptureResult("clear-native-session"),
			"Clear revokes logical delivery but cannot erase a retained native charge.");
		state->emitResult = true; renderer.ProcessQueuedCaptureRequests();
		context.Expect(!renderer.PollCaptureResult("clear-native-session") && renderer.GetCaptureQueueState().activeCaptureCount == 1,
			"Old-session terminal output must remain revoked across Clear.");
		state->retained = false; renderer.ProcessQueuedCaptureRequests();
		context.ExpectEqual(0u, renderer.GetCaptureQueueState().activeCaptureCount, "Only native release may free the old-session charge.");
		RenderCaptureBackendTestAccess::PublishCamera(renderer);
		const auto later = renderer.CaptureImage(BackendRequest("clear-native-session"));
		context.Expect(later.errorCategory == "fake_completion_failure" && state->admissions == 2 && state->admissionId != oldAdmission,
			"Reusing a public ID after Clear must correlate a fresh admission instead of reviving old output.");
	});
	registry.Add("Renderer.SyntheticCapture.FakeReadbackFailedShutdownRefusesReplacementUntilProof", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto old = std::make_shared<FakeReadbackState>(); old->shutdownAllowed = false; FakeCleanup cleanup{old};
		context.Expect(RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(old)), "Fake backend must attach.");
		std::string error; renderer.QueueCaptureRequest(BackendRequest("unproven-native-owner"), error); renderer.ProcessQueuedCaptureRequests();
		auto* original = renderer.GetContext(); renderer.Shutdown();
		context.Expect(renderer.GetContext() == original && old->destroyed == 0 && renderer.GetCaptureQueueState().activeCaptureCount == 1,
			"Failed idle proof must retain exact context and physical ledger during explicit Shutdown.");
		auto candidate = std::make_shared<FakeReadbackState>();
		context.Expect(!RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(candidate))
			&& renderer.GetContext() == original && old->destroyed == 0 && candidate->destroyed == 1,
			"Production replacement gate must refuse overwrite while old native proof is missing.");
		old->shutdownAllowed = true;
		auto fresh = std::make_shared<FakeReadbackState>(); FakeCleanup freshCleanup{fresh};
		context.Expect(RenderCaptureBackendTestAccess::Attach(renderer, std::make_unique<FakeReadbackBackend>(fresh)) && old->destroyed == 1
			&& renderer.GetCaptureQueueState().activeCaptureCount == 0, "Retrying the old owner must prove retirement before replacement.");
		fresh->emitResult = true; fresh->retained = false;
		context.Expect(renderer.CaptureImage(BackendRequest("new-backend-session")).errorCategory == "fake_completion_failure" && fresh->admissions == 1,
			"Fresh backend must receive only its own new admission after recovery.");
	});
	registry.Add("Renderer.SyntheticCapture.ClearCancelsQueuedBeforeDispatchAndAllowsHealthyWork", [](TestContext& context) {
		RendererFixture fixture; auto& renderer = RenderSystem::Instance();
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>();
		SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		std::string error; renderer.QueueCaptureRequest(Request("queued-cancellation"), error); renderer.Clear();
		const auto state = renderer.GetCaptureQueueState();
		context.Expect(!checkpoint->entered.load() && state.pendingCaptureCount == 0 && state.activeCaptureCount == 0
			&& !renderer.PollCaptureResult("queued-cancellation"), "Clear before dispatch must revoke queue ownership without starting an encoder or publishing stale output.");
		context.Expect(renderer.CaptureImage(Request("healthy-after-queued-cancellation")).ok, "Queued cancellation must leave later capture usable.");
	});
	struct GuardCase { const char* name; glm::uvec2 size; const char* reason; };
	// These are legal PNG dimension values on MSVC x64. Their exact products
	// respectively reach RGBA, filtered-row and DEFLATE capacity guards, before
	// any pixel allocation. A fabricated enormous span is never constructed.
	for (const auto guard : {GuardCase{"PixelCapacity", {2147483647u,2147483647u}, "RGBA capture"},
		GuardCase{"FilteredRowCapacity", {1073741825u,2147483646u}, "filtered rows"},
		GuardCase{"DeflateCapacity", {1073741824u,2147483647u}, "DEFLATE stream"}}) {
		registry.Add(std::string("Renderer.SyntheticCapture.PublicPreflight") + guard.name, [guard](TestContext& context) {
			RendererFixture fixture; auto& renderer = RenderSystem::Instance();
			auto request = Request("legal-dimensions-overflow"); request.targetSize = guard.size;
			const auto rejected = renderer.CaptureImage(request);
			context.Expect(!rejected.ok && rejected.base64Data.empty() && rejected.message.find(guard.reason) != std::string::npos,
				"Public admission must report the specific real arithmetic capacity guard before allocating or encoding.");
			const auto state = renderer.GetCaptureQueueState();
			context.Expect(state.pendingCaptureCount == 0 && state.activeCaptureCount == 0 && !renderer.PollCaptureResult(request.requestId),
				"Preflight rejection must own no queue/job charge and produce no second result.");
			context.Expect(renderer.CaptureImage(Request("healthy-after-preflight")).ok, "Real capacity rejection must not wedge later valid work.");
		});
	}
	registry.Add("Renderer.SyntheticCapture.CheckedLayoutRejectsFormatAndStorageOverflow", [](TestContext& context) {
		for (const auto extent : {glm::uvec2(0,1), glm::uvec2(UINT32_MAX,UINT32_MAX), glm::uvec2(0x7fffffff,1)}) {
			bool rejected = false;
			try { (void)ValidateRgba8PngLayout(extent.x, extent.y); } catch (const std::exception&) { rejected = true; }
			context.Expect(rejected, "Invalid dimensions and single-IDAT overflow must reject without allocating pixels.");
		}
		const auto layout = ValidateRgba8PngLayout(4096,4096);
		context.Expect(layout.rgbaBytes == 4096ull * 4096 * 4 && layout.pngBytes > layout.rgbaBytes && layout.base64Bytes > layout.pngBytes,
			"Representable large dimensions must remain supported without a policy cap.");
	});
	// All four guards are reachable through actual public preflight on x64 and
	// reject before pixel/job allocation; no enormous span or pointer is forged.
	struct PublicFailureGuard { const char* name; glm::uvec2 extent; const char* reason; };
	for(const auto guard : {PublicFailureGuard{"ZeroWidth", {0U,2U}, "non-zero"},
		PublicFailureGuard{"ZeroHeight", {2U,0U}, "non-zero"},
		PublicFailureGuard{"PngDimensionBound", {UINT32_MAX,1U}, "format bounds"},
		PublicFailureGuard{"SingleIdatChunkBound", {0x7fffffffU,1U}, "IDAT chunk"}}) {
		registry.Add(std::string("Renderer.SyntheticCapture.PublicFailureSupplement.") + guard.name, [guard](TestContext& context) {
			RendererFixture fixture; auto& renderer = RenderSystem::Instance();
			auto request = Request(std::string("public-failure-") + guard.name); request.targetSize = guard.extent;
			const auto failed = renderer.CaptureImage(request);
			context.Expect(!failed.ok && failed.base64Data.empty() && !failed.pendingAsync && !failed.errorCategory.empty()
				&& failed.requestId == request.requestId && failed.sourceKind == request.sourceKind
				&& failed.sourceName == request.sourceName && failed.size == request.targetSize
				&& failed.message.find(guard.reason) != std::string::npos,
				"Public failure must preserve request context, report its exact guard and contain no successful image.");
			const auto state = renderer.GetCaptureQueueState();
			context.Expect(state.pendingCaptureCount == 0 && state.activeCaptureCount == 0 && state.completedResultCount == 0,
				"Rejected preflight must retain no queue charge, physical job or deferred terminal result.");
			for(unsigned poll = 0; poll < 8; ++poll) {
				renderer.ProcessQueuedCaptureRequests();
				context.Expect(!renderer.PollCaptureResult(request.requestId), "Consumed public rejection must not publish another result.");
			}
			CheckHealthyAfterFailure(context, renderer, std::string("healthy-after-") + guard.name);
			context.Expect(!renderer.PollCaptureResult(request.requestId), "Recovery must not resurrect a rejected admission.");
			std::cout << "CAPTURE_FAILURE_OBSERVATION " << nlohmann::json({{"case", guard.name},
				// Record actual host limits for the unreachable later arithmetic guards;
				// these empty containers query capacity without allocating large buffers.
				{"sizeTBits", std::numeric_limits<std::size_t>::digits},
				{"ptrdiffMax", std::numeric_limits<std::ptrdiff_t>::max()},
				{"vectorMaxSize", std::vector<std::uint8_t>{}.max_size()}, {"stringMaxSize", std::string{}.max_size()},
				{"requestedWidth", request.targetSize.x}, {"requestedHeight", request.targetSize.y},
				{"errorCategory", failed.errorCategory}, {"guard", guard.reason}, {"emptyImage", failed.base64Data.empty()},
				{"pendingAfterFailure", state.pendingCaptureCount}, {"activeAfterFailure", state.activeCaptureCount},
				{"completedAfterFailure", state.completedResultCount}, {"duplicatePolls", 8}, {"healthyDecoded", true}}).dump() << '\n';
		});
	}
	for(const bool shutdown : {false,true}) {
		registry.Add(std::string("Renderer.SyntheticCapture.PublicFailureSupplement.Sync") + (shutdown ? "Shutdown" : "Clear"), [shutdown](TestContext& context) {
			RendererFixture fixture; auto& renderer = RenderSystem::Instance();
			auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = true;
			SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
			auto request = Request(shutdown ? "sync-shutdown-revocation" : "sync-clear-revocation");
			request.freshness = "nextFrame";
			std::promise<RenderCaptureResult> completion; auto future = completion.get_future();
			std::atomic<bool> callerReturned{false};
			std::jthread caller([&] {
				try { completion.set_value(renderer.CaptureImage(request)); }
				catch(...) { completion.set_exception(std::current_exception()); }
				callerReturned.store(true, std::memory_order_release);
			});
			// Unwinding releases the held worker before caller's jthread joins.
			// Request/promise inputs and the renderer fixture outlive both threads.
			ReleaseGate release{checkpoint};
			context.Expect(Wait([&] { return checkpoint->entered.load(); }), "A real admitted CPU worker must own the synchronous request.");
			context.Expect(!callerReturned.load(std::memory_order_acquire),
				"Lifecycle revocation must occur while the synchronous facade is still waiting.");
			context.ExpectEqual(1U, renderer.GetCaptureQueueState().activeCaptureCount, "Held worker must retain its physical charge.");
			std::atomic<bool> observedRetirement{false}, returnedWhileJoining{false}, chargeWhileReturning{false};
			std::jthread releaser([&] {
				const bool retiring = Wait([&] { return SyntheticCaptureTestAccess::IsRetiringCaptures(renderer); });
				observedRetirement.store(retiring);
				if(retiring) {
					returnedWhileJoining.store(Wait([&] { return callerReturned.load(std::memory_order_acquire); }));
					chargeWhileReturning.store(renderer.GetCaptureQueueState().activeCaptureCount == 1U && !checkpoint->retired.load());
				}
				// Always release after the bounded observation, including a failed
				// admission/expectation, so teardown cannot leave a detached worker.
				release.Release();
			});
			const bool retired = Wait([&] {
				try { if(shutdown) renderer.Shutdown(); else renderer.Clear(); return true; }
				catch(const std::logic_error& error) {
					if(std::string_view(error.what()) != "Render frame execution is already active") throw;
					return false;
				}
			});
			releaser.join();
			context.Expect(retired && observedRetirement.load() && checkpoint->retired.load(),
				"Lifecycle must own an actual protected retirement episode and physically settle the held worker.");
			context.Expect(returnedWhileJoining.load() && chargeWhileReturning.load(),
				"Stats-only revocation must return promptly while lifecycle still owns the worker's physical charge.");
			context.Expect(callerReturned.load(std::memory_order_acquire), "Revoked facade must return without waiting for its polling deadline.");
			caller.join();
			const auto failed = future.get();
			context.Expect(!failed.ok && failed.errorCategory == "cancelled" && failed.base64Data.empty() && !failed.pendingAsync
				&& failed.requestId == request.requestId && failed.sourceKind == request.sourceKind
				&& failed.sourceName == request.sourceName && failed.size == request.targetSize && failed.freshness == request.freshness,
				"Retired synchronous admission must return one owned cancelled failure with exact context.");
			const auto state = renderer.GetCaptureQueueState();
			context.Expect(state.pendingCaptureCount == 0 && state.activeCaptureCount == 0 && state.completedResultCount == 0,
				"Joined lifecycle revocation must release every physical and logical queue charge.");
			for(unsigned poll = 0; poll < 8; ++poll) {
				renderer.ProcessQueuedCaptureRequests();
				context.Expect(!renderer.PollCaptureResult(request.requestId), "Revoked asynchronous delivery must remain absent after synchronous cancellation.");
			}
			CheckHealthyAfterFailure(context, renderer, shutdown ? "healthy-after-sync-shutdown" : "healthy-after-sync-clear");
			std::cout << "CAPTURE_FAILURE_OBSERVATION " << nlohmann::json({{"case", shutdown ? "SyncShutdown" : "SyncClear"},
				{"retirementObserved", observedRetirement.load()}, {"returnedWhileJoining", returnedWhileJoining.load()},
				{"physicalChargeWhileReturning", chargeWhileReturning.load()}, {"physicalOwnerRetired", checkpoint->retired.load()},
				{"errorCategory", failed.errorCategory}, {"emptyImage", failed.base64Data.empty()},
				{"activeAfterRetirement", state.activeCaptureCount}, {"duplicatePolls", 8}, {"healthyDecoded", true}}).dump() << '\n';
		});
	}

	registry.Add("Renderer.SyntheticCapture.PublicFailureSupplement.ExecutorUnavailable", [](TestContext& context) {
		// A public missing-executor test must not stop a pool owned by another
		// fixture. The recovery scope creates and retires only its own executor.
		context.ExpectEqual(std::size_t(0), ThreadPool::instance().getThreadCount(), "Public missing-executor qualification requires a quiescent singleton.");
		auto& renderer = RenderSystem::Instance(); renderer.Clear();
		struct CaptureCleanup { ~CaptureCleanup() { RenderSystem::Instance().Clear(); } } cleanup;
		auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>();
		SyntheticCaptureTestAccess::SetNextCheckpoint(renderer, checkpoint);
		const auto request = Request("public-missing-executor");
		const auto failed = renderer.CaptureImage(request);
		context.Expect(!failed.ok && failed.errorCategory == "capture_admission_failed" && failed.base64Data.empty() && !failed.pendingAsync
			&& failed.requestId == request.requestId && failed.sourceKind == request.sourceKind && failed.sourceName == request.sourceName
			&& failed.size == request.targetSize && !checkpoint->entered.load(),
			"Missing executor must produce one owned empty public failure before any CPU job enters.");
		const auto state = renderer.GetCaptureQueueState();
		context.Expect(state.pendingCaptureCount == 0 && state.activeCaptureCount == 0 && state.completedResultCount == 0,
			"Missing executor must leave no queue, physical job or deferred result.");
		for(unsigned poll = 0; poll < 8; ++poll) {
			renderer.ProcessQueuedCaptureRequests();
			context.Expect(!renderer.PollCaptureResult(request.requestId), "Public executor failure must not publish a second result.");
		}
		{
			RendererFixture recovery;
			CheckHealthyAfterFailure(context, renderer, "healthy-after-public-missing-executor");
		}
		context.Expect(ThreadPool::instance().getThreadCount() == 0 && !renderer.PollCaptureResult(request.requestId),
			"Healthy retry must drain its owned executor without resurrecting the failed admission.");
		std::cout << "CAPTURE_FAILURE_OBSERVATION " << nlohmann::json({{"case", "ExecutorUnavailable"},
			{"errorCategory", failed.errorCategory}, {"emptyImage", failed.base64Data.empty()}, {"workerEntered", checkpoint->entered.load()},
			{"activeAfterFailure", state.activeCaptureCount}, {"duplicatePolls", 8}, {"healthyDecoded", true},
			{"workersAfterRecovery", ThreadPool::instance().getThreadCount()}}).dump() << '\n';
	});

}
}
