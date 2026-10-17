#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <iterator>
#include <utility>
#include "Core/Export.h"

namespace TerraEngine::Renderer::Vulkan
{
	inline constexpr std::size_t kReadbackOperationCapacity = 4;
	// Image capture and uint4 picking borrow the same native resource kinds. Their
	// payload adapters differ; all ownership decisions pass through this controller.
	enum class ReadbackKind : std::uint8_t { Capture, VisibilityIdentity };
	enum class ReadbackClientState : std::uint8_t { Pending, TerminalPublished };
	enum class ReadbackNativeState : std::uint8_t { Prepared, Submitted, RetirementAuthorized, Retired };
	enum class ReadbackFenceObservation : std::uint8_t { NotReady, Completed, DeviceLost, QueryError };
	enum class ReadbackTerminalReason : std::uint8_t { Completed, Timeout, Canceled, DeviceLost, QueryError };
	enum class ReadbackRetirementProof : std::uint8_t { None, SubmissionRejected, FenceCompleted, DeviceIdle, DeviceLost };

	class ReadbackLifecycleAdapter
	{
	public:
		virtual ~ReadbackLifecycleAdapter() = default;
		// The production adapter queries the actual fence. Tests substitute only
		// the observation and native side effects, never the ownership policy.
		virtual ReadbackFenceObservation QueryFence() = 0;
		// Return only after delivery storage owns the terminal result. Throwing
		// leaves the client pending so the next protected poll can retry delivery.
		virtual void PublishTerminal(ReadbackTerminalReason reason) = 0;
		// Native destruction must finish once authorized. A throwing callback
		// would strand RetirementAuthorized and could leave partial owner release.
		virtual void RetireNative() noexcept = 0;
	};

	class TERRAENGINE_API ReadbackLifecycle
	{
	public:
		explicit ReadbackLifecycle(ReadbackKind kind = ReadbackKind::Capture) noexcept : kind_(kind) {}
		void AcceptSubmission() noexcept;
		void Poll(ReadbackLifecycleAdapter& adapter, bool deadlineExpired);
		void Cancel(ReadbackLifecycleAdapter& adapter, ReadbackRetirementProof proof, bool publishResult);
		void RejectSubmission(ReadbackLifecycleAdapter& adapter) noexcept;
		[[nodiscard]] bool BlocksSourceImage() const noexcept;
		[[nodiscard]] ReadbackClientState ClientState() const noexcept { return client_; }
		[[nodiscard]] ReadbackNativeState NativeState() const noexcept { return native_; }
		[[nodiscard]] ReadbackRetirementProof RetirementProof() const noexcept { return proof_; }

	private:
		void Publish(ReadbackLifecycleAdapter& adapter, ReadbackTerminalReason reason);
		void Retire(ReadbackLifecycleAdapter& adapter, ReadbackRetirementProof proof) noexcept;
		ReadbackKind kind_;
		ReadbackClientState client_ = ReadbackClientState::Pending;
		ReadbackNativeState native_ = ReadbackNativeState::Prepared;
		ReadbackRetirementProof proof_ = ReadbackRetirementProof::None;
	};

	// Both native admission paths use this guard while preparing metadata and
	// submitting. An exception or rejected submit reaches the same production
	// RejectSubmission transition; accepted work transfers only typed state.
	template<typename Rollback>
	class ScopedReadbackAdmission final : private ReadbackLifecycleAdapter
	{
		ReadbackLifecycle prepared_;
		Rollback rollback_;
		bool accepted_ = false;
		ReadbackFenceObservation QueryFence() override { return ReadbackFenceObservation::NotReady; }
		void PublishTerminal(ReadbackTerminalReason) override {}
		void RetireNative() noexcept override { rollback_(); }
	public:
		explicit ScopedReadbackAdmission(Rollback rollback) noexcept : rollback_(std::move(rollback)) {
			static_assert(std::is_nothrow_move_constructible_v<Rollback>);
			static_assert(std::is_nothrow_invocable_v<Rollback&>);
		}
		ScopedReadbackAdmission(const ScopedReadbackAdmission&) = delete;
		ScopedReadbackAdmission& operator=(const ScopedReadbackAdmission&) = delete;
		~ScopedReadbackAdmission() noexcept override {
			if (!accepted_) prepared_.RejectSubmission(*this);
		}
		void Accept(ReadbackLifecycle& retainedOperation) noexcept {
			retainedOperation.AcceptSubmission();
			accepted_ = true;
		}
	};

	// Slots never relocate an admitted operation. Construction may fail before
	// submission, but accepted submission only changes the record's typed state.
	// Erasing a slot destroys CPU bookkeeping only; native destruction is always
	// an explicit controller/adapter action with a completion proof.
	template<typename T, std::size_t Capacity>
	class ReadbackOperationSlots
	{
		template<bool Constant>
		class Iterator
		{
			using Slot = std::conditional_t<Constant, const std::optional<T>, std::optional<T>>;
			Slot* current_;
			Slot* end_;
			void SkipEmpty() noexcept { while (current_ != end_ && !current_->has_value()) ++current_; }
			friend class ReadbackOperationSlots;
			Iterator(Slot* current, Slot* end) noexcept : current_(current), end_(end) { SkipEmpty(); }
		public:
			Iterator() noexcept : current_(nullptr), end_(nullptr) {}
			using iterator_category = std::forward_iterator_tag;
			using value_type = T;
			using difference_type = std::ptrdiff_t;
			using reference = std::conditional_t<Constant, const T&, T&>;
			using pointer = std::conditional_t<Constant, const T*, T*>;
			reference operator*() const noexcept { return current_->value(); }
			pointer operator->() const noexcept { return &current_->value(); }
			Iterator& operator++() noexcept { ++current_; SkipEmpty(); return *this; }
			Iterator operator++(int) noexcept { auto previous = *this; ++*this; return previous; }
			bool operator!=(const Iterator& other) const noexcept { return current_ != other.current_; }
			bool operator==(const Iterator& other) const noexcept { return current_ == other.current_; }
		};
		std::array<std::optional<T>, Capacity> slots_{};
	public:
		using iterator = Iterator<false>;
		using const_iterator = Iterator<true>;
		iterator begin() noexcept { return { slots_.data(), slots_.data() + Capacity }; }
		iterator end() noexcept { return { slots_.data() + Capacity, slots_.data() + Capacity }; }
		const_iterator begin() const noexcept { return { slots_.data(), slots_.data() + Capacity }; }
		const_iterator end() const noexcept { return { slots_.data() + Capacity, slots_.data() + Capacity }; }
		[[nodiscard]] std::size_t size() const noexcept {
			std::size_t count = 0;
			for (const auto& slot : slots_) if (slot) ++count;
			return count;
		}
		T& emplace_back() {
			for (auto& slot : slots_) if (!slot) return slot.emplace();
			throw std::length_error("Bounded readback ownership slots are full.");
		}
		iterator erase(iterator position) noexcept {
			position.current_->reset();
			return ++position;
		}
		void erase(T* record) noexcept {
			for (auto& slot : slots_) if (slot && &*slot == record) { slot.reset(); return; }
		}
		void clear() noexcept { for (auto& slot : slots_) slot.reset(); }
	};
}
