#include "Renderer/SyntheticCapture.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "ThreadPool/ThreadPool.h"
#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

namespace TerraEngine::Renderer {
namespace {
	RenderCaptureResult FailureResult(const RenderCaptureRequest& request, std::uint64_t frameId)
	{
		RenderCaptureResult result;
		result.requestId = request.requestId;
		result.sourceKind = request.sourceKind;
		result.sourceName = request.sourceName;
		result.size = request.targetSize;
		result.frameId = frameId;
		result.freshness = request.freshness;
		result.message = "Synthetic capture preparation failed; no scene pixels were captured.";
		result.errorCategory = "capture_encoding_failed";
		return result;
	}
	struct CompletionState { std::atomic<bool> retired{false}; };
	struct RetirementSignal
	{
		std::shared_ptr<CompletionState> completion;
		std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint;
		~RetirementSignal()
		{
			// WorkOwner declares this first, so all copied inputs have already
			// died when this last member releases the physical capacity charge.
			if (!completion) return;
			if (checkpoint && checkpoint->holdBeforeRetirement && checkpoint->entered.load(std::memory_order_acquire)) {
				checkpoint->retirementEntered.store(true, std::memory_order_release);
				while (!checkpoint->retirementReleased.load(std::memory_order_acquire)) checkpoint->retirementReleased.wait(false, std::memory_order_acquire);
			}
			if (checkpoint) { checkpoint->retired.store(true, std::memory_order_release); checkpoint->retired.notify_all(); }
			completion->retired.store(true, std::memory_order_release);
			completion->retired.notify_all();
		}
	};
	struct WorkOwner
	{
		RetirementSignal retirement;
		RenderCaptureRequest request;
		std::uint64_t frameId = 0;
		std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint;
	};
}

RenderCaptureResult PrepareSyntheticCapture(const RenderCaptureRequest& request, std::uint64_t frameId, SyntheticCaptureFault fault)
{
	// Construct failure delivery before allocating potentially large buffers. The
	// catch path only clears bytes and returns this already-owned failure record.
	auto result = FailureResult(request, frameId);
	try {
		if (request.format != "image/png" || !request.allowBackendFallback) return result;
		const auto layout = ValidateRgba8PngLayout(request.targetSize.x, request.targetSize.y);
		std::vector<std::uint8_t> pixels(layout.rgbaBytes, 0);
		if (!request.transparentBackground) {
			for (std::size_t alpha = 3; alpha < pixels.size(); alpha += 4) pixels[alpha] = 255;
		}
		const auto png = EncodeRgba8ToPng(pixels, request.targetSize.x, request.targetSize.y);
		result.base64Data = EncodeBase64(png);
		if (fault == SyntheticCaptureFault::LateEncoding) throw std::runtime_error("Synthetic precommit checkpoint failure.");
		std::string successMessage = "Prepared a deterministic synthetic black image; frame metadata identifies context, not rendered scene content.";
		result.warnings.push_back("Synthetic image does not contain live GPU or scene pixels.");
		if (request.includeOverlays) result.warnings.push_back("Requested overlays are unavailable in synthetic images.");
		result.message.swap(successMessage);
		result.errorCategory.clear();
		result.deterministicFallback = true;
		result.ok = true;
	} catch (...) {
		result.base64Data.clear();
		result.ok = false;
		result.deterministicFallback = false;
	}
	return result;
}

struct SyntheticCaptureJob::Impl
{
	std::shared_ptr<CompletionState> completion;
	std::future<RenderCaptureResult> future;
	RenderCaptureResult failure;
	bool consumed = false;
};
SyntheticCaptureJob::SyntheticCaptureJob() = default;
SyntheticCaptureJob::~SyntheticCaptureJob() { Join(); }
SyntheticCaptureJob::SyntheticCaptureJob(SyntheticCaptureJob&&) noexcept = default;
SyntheticCaptureJob& SyntheticCaptureJob::operator=(SyntheticCaptureJob&& other) noexcept
{
	if (this != &other) { Join(); impl = std::move(other.impl); }
	return *this;
}
bool SyntheticCaptureJob::Start(RenderCaptureRequest request, std::uint64_t frameId, ThreadPool* executor,
	std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint, std::string& error)
{
	if (impl) { error = "Synthetic job already admitted."; return false; }
	if (checkpoint && checkpoint->holdBeforeRetirement && !checkpoint->holdBeforeEncoding) {
		error = "Retirement validation requires an initial owned worker hold."; return false;
	}
	try {
		(void)ValidateRgba8PngLayout(request.targetSize.x, request.targetSize.y);
		if (!executor || executor->getThreadCount() == 0) { error = "Synthetic capture requires an initialized executor."; return false; }
		if (checkpoint && checkpoint->fault == SyntheticCaptureFault::Admission) { error = "Synthetic capture admission failed."; return false; }
		auto prepared = std::make_unique<Impl>();
		prepared->failure = FailureResult(request, frameId);
		prepared->completion = std::make_shared<CompletionState>();
		auto owner = std::make_shared<WorkOwner>();
		owner->retirement.completion = prepared->completion;
		owner->retirement.checkpoint = checkpoint;
		owner->request = std::move(request);
		owner->frameId = frameId;
		owner->checkpoint = std::move(checkpoint);
		auto task = executor->createTask([owner]() -> RenderCaptureResult {
			if (owner->checkpoint) {
				owner->checkpoint->entered.store(true, std::memory_order_release);
				owner->checkpoint->entered.notify_all();
				if (owner->checkpoint->holdBeforeEncoding) {
					while (!owner->checkpoint->released.load(std::memory_order_acquire)) owner->checkpoint->released.wait(false, std::memory_order_acquire);
				}
				if (owner->checkpoint->fault == SyntheticCaptureFault::Allocation) throw std::bad_alloc();
				if (owner->checkpoint->fault == SyntheticCaptureFault::Encoding) throw std::runtime_error("Synthetic encoding checkpoint failure.");
			}
			return PrepareSyntheticCapture(owner->request, owner->frameId, owner->checkpoint ? owner->checkpoint->fault : SyntheticCaptureFault::None);
		});
		prepared->future = task->getFuture();
		// This is the only throwing admission. Rejection owns no queued task;
		// local task/owner destruction precedes any future wait. After acceptance
		// the no-throw pointer move publishes preallocated tracking immediately.
		if (owner->checkpoint && owner->checkpoint->fault == SyntheticCaptureFault::Enqueue) throw std::runtime_error("Synthetic enqueue checkpoint rejection.");
		executor->enqueue(task);
		impl = std::move(prepared);
		// Neither the future nor CompletionState owns Task/WorkOwner. The pool
		// drops the callable after run/Fail, independently signalling retirement.
		error.clear();
		return true;
	} catch (const std::exception& exception) { error = exception.what(); return false; }
	catch (...) { error = "Synthetic capture admission failed."; return false; }
}
std::optional<RenderCaptureResult> SyntheticCaptureJob::TakeResult()
{
	if (!impl || impl->consumed || impl->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
	impl->consumed = true;
	try { return impl->future.get(); }
	catch (...) { return std::move(impl->failure); }
}
bool SyntheticCaptureJob::IsPhysicallySettled() const noexcept
{
	return !impl || impl->completion->retired.load(std::memory_order_acquire);
}
void SyntheticCaptureJob::Join() noexcept
{
	if (!impl) return;
	// No Task is retained here: rejected tasks were destroyed before Start
	// returned, and shutdown resolves accepted unstarted tasks through Fail.
	if (impl->future.valid()) { impl->consumed = true; try { impl->future.get(); } catch (...) {} }
	while (!impl->completion->retired.load(std::memory_order_acquire)) impl->completion->retired.wait(false, std::memory_order_acquire);
}
}
