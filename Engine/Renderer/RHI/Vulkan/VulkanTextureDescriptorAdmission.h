#pragma once

#include <cstdint>
#include <thread>
#include <utility>

namespace TerraEngine::Renderer
{
	// Bootstrap grants belong to one protected initialization and never make
	// an otherwise uninitialized backend available to ordinary runtime work.
	struct TextureDescriptorBootstrap
	{
		bool active = false;
		std::thread::id owner;
		std::uintptr_t device = 0;
		std::uint64_t session = 0;
		std::uint8_t reservedWrites = 0;
		bool failed = false;
		void Reset() noexcept { *this = {}; }
		bool Matches(std::uintptr_t currentDevice, std::uint64_t currentSession) const noexcept
		{
			return active && owner == std::this_thread::get_id()
				&& device == currentDevice && session == currentSession;
		}
		bool RequiredWritesComplete() const noexcept { return active && !failed && reservedWrites == 0x7u; }
		void RecordWrite(std::uint32_t index, bool written) noexcept
		{
			if (!active) return;
			if (!written) failed = true;
			else if (index < 3u) reservedWrites |= static_cast<std::uint8_t>(1u << index);
		}
	};
	class ScopedTextureDescriptorBootstrap
	{
	public:
		ScopedTextureDescriptorBootstrap(TextureDescriptorBootstrap& state,
			std::uintptr_t device, std::uint64_t session, bool ready) : state_(state)
		{
			if (state_.active) { state_.failed = true; return; }
			owns_ = true;
			state_.Reset();
			state_.active = ready && device != 0 && session != 0;
			state_.owner = std::this_thread::get_id();
			state_.device = device;
			state_.session = session;
		}
		~ScopedTextureDescriptorBootstrap() { if (owns_) state_.Reset(); }
		ScopedTextureDescriptorBootstrap(const ScopedTextureDescriptorBootstrap&) = delete;
		ScopedTextureDescriptorBootstrap& operator=(const ScopedTextureDescriptorBootstrap&) = delete;
	private:
		TextureDescriptorBootstrap& state_;
		bool owns_ = false;
	};
	enum class TextureDescriptorAdmission { Reject, Bootstrap, RuntimeWait };
	struct TextureDescriptorAdmissionProof
	{
		bool initialized = false;
		bool deviceReady = false;
		bool deviceLost = false;
		bool setsReady = false;
		bool pendingWork = false;
		bool bootstrapOwnerCurrent = false;
	};
	inline TextureDescriptorAdmission DecideTextureDescriptorAdmission(
		const TextureDescriptorAdmissionProof& proof) noexcept
	{
		// Runtime retains its idle proof. Only the current protected bootstrap
		// owner can write before readiness, when no frame work exists to wait on.
		if (!proof.deviceReady || proof.deviceLost || !proof.setsReady)
			return TextureDescriptorAdmission::Reject;
		if (proof.initialized) return TextureDescriptorAdmission::RuntimeWait;
		return proof.bootstrapOwnerCurrent && !proof.pendingWork
			? TextureDescriptorAdmission::Bootstrap : TextureDescriptorAdmission::Reject;
	}
	template<class Wait, class Write>
	bool ApplyTextureDescriptorAdmission(TextureDescriptorAdmission admission, Wait&& wait, Write&& write)
	{
		if (admission == TextureDescriptorAdmission::Reject) return false;
		if (admission == TextureDescriptorAdmission::RuntimeWait && !std::forward<Wait>(wait)()) return false;
		std::forward<Write>(write)();
		return true;
	}
}
