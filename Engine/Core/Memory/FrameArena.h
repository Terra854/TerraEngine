// FrameArena.h — Linear allocator for high-performance, frame-scoped memory management.
//
// This file implements a singleton arena allocator that provides fast, contiguous
// memory blocks that are reset at the end of each frame. It includes an
// STL-compatible allocator wrapper, allowing engine systems and containers to
// perform transient allocations with zero individual deallocation cost.

#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <cassert>

class FrameArena {
public:
	// Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static FrameArena& Instance() {
		static FrameArena instance;
		return instance;
	}

	FrameArena(const FrameArena&) = delete;
	FrameArena& operator=(const FrameArena&) = delete;

	// Allocate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* Allocate(size_t size, size_t alignment = 8) {
		// Align the current pointer
		size_t padding = (alignment - (offset % alignment)) % alignment;
		size_t newOffset = offset + padding;
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (newOffset + size > bufferSize) {
			// Fallback to heap if arena is full (or handle error)
			// For now, let's just assert or expand. Expanding implies a list of buffers.
			// Simple implementation: return malloc if full (leaked until frame end? no, this is a linear allocator)
			// Better: Resize buffer. But that invalidates pointers?
			// Strategy: Multiple blocks.
			return AllocateSlow(size, alignment);
		}
		
		void* ptr = buffer.data() + newOffset;
		offset = newOffset + size;
		return ptr;
	}

	template<typename T>
	// NewArray is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	T* NewArray(size_t count) {
		return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
	}

	// Reset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Reset() {
		offset = 0;
		// Free fallback blocks if any
		fallbackBlocks.clear(); 
	}

private:
	FrameArena() {
		bufferSize = 1024 * 1024 * 16; // 16MB default frame buffer
		buffer.resize(bufferSize);
	}

	// AllocateSlow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void* AllocateSlow(size_t size, size_t alignment) {
		// Fallback allocation strategy for when the main block is full.
		// We create a dedicated block for this allocation.
		// This is slower but safe.
		std::vector<uint8_t> newBlock(size + alignment);
		void* ptr = newBlock.data();
		size_t space = newBlock.size();
		std::align(alignment, size, ptr, space);
		
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
	// FrameAllocator is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	template <typename U> FrameAllocator(const FrameAllocator<U>&) {}

	T* allocate(size_t n) {
		return static_cast<T*>(FrameArena::Instance().Allocate(n * sizeof(T), alignof(T)));
	}

	// deallocate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void deallocate(T*, size_t) {
		// No-op: memory is freed when arena resets
	}
};

template <typename T, typename U>
bool operator==(const FrameAllocator<T>&, const FrameAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const FrameAllocator<T>&, const FrameAllocator<U>&) { return false; }
