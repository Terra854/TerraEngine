// FrameArena.h — Linear allocator for high-performance, frame-scoped memory management.
//
// This file implements a singleton arena allocator that provides fast, contiguous
// memory blocks that are reset at the end of each frame. It includes an
// STL-compatible allocator wrapper, allowing engine systems and containers to
// perform transient allocations with zero individual deallocation cost.

#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

namespace TerraEngine::Memory::Detail {
	struct FrameArenaTestAccess;
	// Both the singleton and controlled-span regressions use this exact fast path.
	// The caller owns backing storage and must retire every borrower before reset.
	inline void* TryAllocateFrameArenaSpan(std::uint8_t* backing, size_t capacity,
		size_t& position, size_t bytes, size_t alignment) {
		// Align the actual address, since backing itself need not satisfy an
		// over-aligned request. Subtraction proves the live range fits before
		// changing the caller's position; rejected spans leave it unchanged.
		// Alignment is a nonzero power of two, validated by Allocate or supplied
		// by the controlled-span tests. Empty requests never advance the span.
		if (position > capacity || bytes == 0U) return nullptr;
		size_t remaining = capacity - position;
		if (bytes > remaining) return nullptr;
		void* result = backing + position;
		if (std::align(alignment, bytes, result, remaining) == nullptr) return nullptr;
		position = capacity - remaining + bytes;
		return result;
	}
}

class FrameArena {
public:
	// One coordinator owns this singleton's episode. Individual allocations are
	// not synchronized; callers retire every borrower before the next Reset.
	static FrameArena& Instance() {
		static FrameArena instance;
		return instance;
	}

	FrameArena(const FrameArena&) = delete;
	FrameArena& operator=(const FrameArena&) = delete;

	// Return a complete aligned range, or reject without changing live storage.
	// Zero bytes return null; invalid alignment is rejected even for zero bytes.
	void* Allocate(size_t size, size_t alignment = 8) {
		if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
			throw std::invalid_argument("FrameArena alignment must be a nonzero power of two");
		if (size == 0U) return nullptr;
		if (void* ptr = TerraEngine::Memory::Detail::TryAllocateFrameArenaSpan(
			buffer.data(), bufferSize, offset, size, alignment)) {
			return ptr;
		}
		// Dedicated blocks preserve all earlier pointers. Growing fixed backing
		// in place would invalidate the current episode's live ranges.
		return AllocateSlow(size, alignment);
	}

	template<typename T>
	// Reserve raw storage only; the caller owns element construction/destruction.
	T* NewArray(size_t count) {
		if (count > (std::numeric_limits<size_t>::max)() / sizeof(T)) throw std::bad_array_new_length();
		return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
	}

	// Call only after containers and borrowing jobs have retired. Reset releases
	// fallback blocks and reuses fixed backing; it does not destroy stored objects.
	void Reset() {
		offset = 0;
		// Free fallback blocks if any
		fallbackBlocks.clear();
	}

private:
	friend struct TerraEngine::Memory::Detail::FrameArenaTestAccess;
	// Native tests configure these per-arena checkpoints only while quiescent.
	// They are null in normal execution and never replace a global allocator.
	void (*fallbackAllocationCheckpoint_)(void*) = nullptr;
	void (*fallbackTrackingCheckpoint_)(void*) = nullptr;
	void* fallbackCheckpointContext_ = nullptr;
	FrameArena() {
		bufferSize = 1024 * 1024 * 16; // 16MB default frame buffer
		buffer.resize(bufferSize);
	}

	// Alignment was validated by Allocate. Prove adjusted size and both container
	// bounds before attempting allocation, then publish ownership only on success.
	void* AllocateSlow(size_t size, size_t alignment) {
		const size_t maximumPadding = alignment - 1U;
		if (size > (std::numeric_limits<size_t>::max)() - maximumPadding) throw std::bad_alloc();
		const size_t requiredBytes = size + maximumPadding;
		std::vector<uint8_t> newBlock;
		if (requiredBytes > newBlock.max_size() || fallbackBlocks.size() >= fallbackBlocks.max_size())
			throw std::bad_alloc();
		if (fallbackAllocationCheckpoint_) fallbackAllocationCheckpoint_(fallbackCheckpointContext_);
		newBlock.resize(requiredBytes);
		void* ptr = newBlock.data();
		size_t space = newBlock.size();
		// Never publish an unaligned fallback pointer, even if backing cannot
		// accommodate the requested adjustment. The temporary owns cleanup.
		if (std::align(alignment, size, ptr, space) == nullptr) throw std::bad_alloc();
		
		// A throwing checkpoint or tracking-list growth leaves this local vector
		// responsible for its temporary allocation, preserving existing ownership.
		if (fallbackTrackingCheckpoint_) fallbackTrackingCheckpoint_(fallbackCheckpointContext_);
		fallbackBlocks.push_back(std::move(newBlock));
		return ptr;
	}

	std::vector<uint8_t> buffer;
	size_t bufferSize;
	size_t offset = 0;
	
	// Keep fallback allocations alive until Reset()
	std::vector<std::vector<uint8_t>> fallbackBlocks;
};

// STL-compatible allocator for FrameArena
template <typename T>
class FrameAllocator {
public:
	using value_type = T;

	FrameAllocator() = default;

	template <typename U> FrameAllocator(const FrameAllocator<U>&) {}

	T* allocate(size_t n) {
		// Distinguish an impossible element count from ordinary storage pressure.
		if (n > (std::numeric_limits<size_t>::max)() / sizeof(T)) throw std::bad_array_new_length();
		return static_cast<T*>(FrameArena::Instance().Allocate(n * sizeof(T), alignof(T)));
	}


	void deallocate(T*, size_t) {
		// No-op: memory is freed when arena resets
	}
};

template <typename T, typename U>
bool operator==(const FrameAllocator<T>&, const FrameAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const FrameAllocator<T>&, const FrameAllocator<U>&) { return false; }
