#pragma once
#include <cstddef>
namespace TerraEngine::Renderer::Vulkan {
	// CPU publication requires every actual native stream copy, followed by a
	// protected descriptor publication. A failed copy poisons the entire cohort.
	class UploadAcceptance final {
	public:
		UploadAcceptance(std::size_t required, bool mapped) noexcept : required_(required), mapped_(mapped) {}
		void RecordCopy(bool completed) noexcept { if (!completed || completed_ >= required_) failed_ = true; else ++completed_; }
		bool CanPublish(bool descriptorReady, bool deviceHealthy) const noexcept { return required_ != 0 && mapped_ && !failed_ && completed_ == required_ && descriptorReady && deviceHealthy; }
	private:
		std::size_t required_ = 0, completed_ = 0;
		bool mapped_ = false, failed_ = false;
	};
}
