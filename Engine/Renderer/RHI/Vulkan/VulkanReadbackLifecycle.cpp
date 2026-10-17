#include "VulkanReadbackLifecycle.h"

namespace TerraEngine::Renderer::Vulkan
{
	void ReadbackLifecycle::AcceptSubmission() noexcept
	{
		// Protected admission owns the record exclusively. No allocation or native
		// API call belongs in the accepted-submit state transfer.
		if (native_ == ReadbackNativeState::Prepared) native_ = ReadbackNativeState::Submitted;
	}

	void ReadbackLifecycle::Publish(ReadbackLifecycleAdapter& adapter, ReadbackTerminalReason reason)
	{
		if (client_ == ReadbackClientState::TerminalPublished) return;
		adapter.PublishTerminal(reason);
		client_ = ReadbackClientState::TerminalPublished;
	}

	void ReadbackLifecycle::Retire(ReadbackLifecycleAdapter& adapter, ReadbackRetirementProof proof) noexcept
	{
		if (native_ == ReadbackNativeState::Retired) return;
		proof_ = proof;
		native_ = ReadbackNativeState::RetirementAuthorized;
		adapter.RetireNative();
		native_ = ReadbackNativeState::Retired;
	}

	void ReadbackLifecycle::Poll(ReadbackLifecycleAdapter& adapter, bool deadlineExpired)
	{
		if (native_ != ReadbackNativeState::Submitted) return;
		switch (adapter.QueryFence())
		{
		case ReadbackFenceObservation::NotReady:
			if (kind_ == ReadbackKind::Capture && deadlineExpired) Publish(adapter, ReadbackTerminalReason::Timeout);
			return;
		case ReadbackFenceObservation::Completed:
			Publish(adapter, ReadbackTerminalReason::Completed);
			Retire(adapter, ReadbackRetirementProof::FenceCompleted);
			return;
		case ReadbackFenceObservation::DeviceLost:
			Publish(adapter, ReadbackTerminalReason::DeviceLost);
			Retire(adapter, ReadbackRetirementProof::DeviceLost);
			return;
		case ReadbackFenceObservation::QueryError:
			Publish(adapter, ReadbackTerminalReason::QueryError);
			// An ordinary query error says nothing about accepted queue ownership,
			// for either pixel capture or identity picking. Retry its real fence.
			return;
		}
	}

	void ReadbackLifecycle::Cancel(ReadbackLifecycleAdapter& adapter, ReadbackRetirementProof proof, bool publishResult)
	{
		if (native_ == ReadbackNativeState::Retired) return;
		if (publishResult) Publish(adapter, proof == ReadbackRetirementProof::DeviceLost
			? ReadbackTerminalReason::DeviceLost : ReadbackTerminalReason::Canceled);
		// Cleanup's existing idle/device-loss gate supplies this proof. A client
		// cancellation without that proof cannot release submitted Vulkan owners.
		if (proof == ReadbackRetirementProof::DeviceIdle || proof == ReadbackRetirementProof::DeviceLost) Retire(adapter, proof);
	}

	void ReadbackLifecycle::RejectSubmission(ReadbackLifecycleAdapter& adapter) noexcept
	{
		if (native_ == ReadbackNativeState::Prepared) Retire(adapter, ReadbackRetirementProof::SubmissionRejected);
	}

	bool ReadbackLifecycle::BlocksSourceImage() const noexcept
	{
		return native_ == ReadbackNativeState::Submitted || native_ == ReadbackNativeState::RetirementAuthorized;
	}
}
