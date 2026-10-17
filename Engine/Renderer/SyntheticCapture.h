#pragma once

#include "Core/Export.h"
#include "Renderer/RenderTypes.h"
#include <atomic>
#include <memory>
#include <optional>

class ThreadPool;
class RenderSystem;

namespace TerraEngine::Renderer {
	// Native validation owns these checkpoints per admission. They are never
	// populated by runtime settings or editor/MCP requests, and hold no callbacks.
	enum class SyntheticCaptureFault { None, Admission, Allocation, Encoding, DeliveryReservation, Enqueue, LateEncoding };
	struct SyntheticCaptureCheckpoint
	{
		SyntheticCaptureFault fault = SyntheticCaptureFault::None;
		bool holdBeforeEncoding = false;
		bool holdBeforeRetirement = false;
		std::atomic<bool> entered{false};
		std::atomic<bool> released{false};
		std::atomic<bool> retired{false};
		std::atomic<bool> retirementEntered{false};
		std::atomic<bool> retirementReleased{false};
	};

	// The synchronous helper owns its pixels and returns neutral black pixels at
	// the promised extent. A failed preparation never contains successful bytes.
	TERRAENGINE_API RenderCaptureResult PrepareSyntheticCapture(const RenderCaptureRequest& request, std::uint64_t frameId, SyntheticCaptureFault fault = SyntheticCaptureFault::None);

	class TERRAENGINE_API SyntheticCaptureJob
	{
	public:
		SyntheticCaptureJob();
		~SyntheticCaptureJob();
		SyntheticCaptureJob(SyntheticCaptureJob&&) noexcept;
		SyntheticCaptureJob& operator=(SyntheticCaptureJob&&) noexcept;
		SyntheticCaptureJob(const SyntheticCaptureJob&) = delete;
		SyntheticCaptureJob& operator=(const SyntheticCaptureJob&) = delete;
		// The lifecycle owner serializes executor startup/shutdown with admission.
		// All tracking exists before enqueue; an accepted task is owned by the pool.
		bool Start(RenderCaptureRequest request, std::uint64_t frameId, ThreadPool* executor,
			std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint, std::string& error);
		std::optional<RenderCaptureResult> TakeResult();
		bool IsPhysicallySettled() const noexcept;
		void Join() noexcept;
	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	// Only native tests can install the next owned checkpoint at a quiescent
	// boundary. Friendship avoids adding a product setting or automation control.
	struct TERRAENGINE_API SyntheticCaptureTestAccess
	{
		static void SetNextCheckpoint(RenderSystem& renderer, std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint);
		static bool IsRetiringCaptures(RenderSystem& renderer);
	};
}
