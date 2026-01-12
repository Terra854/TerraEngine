#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <cassert>

class FrameArena {
public:
    static FrameArena& Instance() {
        static FrameArena instance;
        return instance;
    }

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    void* Allocate(size_t size, size_t alignment = 8) {
        // Align the current pointer
        size_t padding = (alignment - (offset % alignment)) % alignment;
        size_t newOffset = offset + padding;
        
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
    T* NewArray(size_t count) {
        return static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
    }

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
    template <typename U> FrameAllocator(const FrameAllocator<U>&) {}

    T* allocate(size_t n) {
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
