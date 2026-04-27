// FlatMap.h — Cache-friendly associative container using open addressing.
//
// This template provides a high-performance hash map implementation that uses
// linear probing and can be paired with custom allocators like the FrameArena.
// It is designed for low-level engine systems where memory locality and minimal
// allocation overhead are critical.

#pragma once

#include <vector>
#include <functional>
#include <algorithm>
#include <optional>
#include "FrameArena.h"

// A cache-friendly hash map using open addressing and linear probing.
// Supports custom allocators (e.g., FrameAllocator).
template <typename K, typename V, typename Allocator = std::allocator<std::pair<K, V>>>
class FlatMap {
public:
	// struct Entry groups related state and operations so callers have one documented contract.
	struct Entry {
		K key;
		V value;
		bool occupied = false;
		bool deleted = false; // Tombstone for deletion handling
	};
	
	// We need a separate container for entries.
	// std::vector is convenient but we need to rebind the allocator for the Entry type.
	using EntryAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Entry>;
	using Container = std::vector<Entry, EntryAllocator>;

	// FlatMap is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	explicit FlatMap(size_t capacity = 16, const Allocator& alloc = Allocator())
		: entries(capacity, alloc), allocator(alloc), size_(0) {}

	// No copy constructor for now to keep it simple, or implement deep copy if needed.
	// Move constructor
	FlatMap(FlatMap&& other) noexcept = default;
	FlatMap& operator=(FlatMap&& other) noexcept = default;

	// clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void clear() {
		for (auto& entry : entries) {
			entry.occupied = false;
			entry.deleted = false;
			// Optionally destruct value if non-trivial
		}
		size_ = 0;
	}

	// reserve is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void reserve(size_t capacity) {
		if (capacity > entries.size()) {
			rehash(capacity);
		}
	}

	V& operator[](const K& key) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (size_ >= entries.size() * 0.7) { // Load factor 0.7
			rehash(std::max(size_t(16), entries.size() * 2));
		}

		size_t idx = FindIndex(key);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (entries[idx].occupied) {
			return entries[idx].value;
		}

		// Insert new
		entries[idx].key = key;
		entries[idx].occupied = true;
		entries[idx].deleted = false;
		entries[idx].value = V{}; // Default construct
		size_++;
		return entries[idx].value;
	}

	// Emplace
	template <typename... Args>
	void emplace(const K& key, Args&&... args) {
		if (size_ >= entries.size() * 0.7) {
			rehash(std::max(size_t(16), entries.size() * 2));
		}

		size_t idx = FindIndex(key);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!entries[idx].occupied) {
			entries[idx].key = key;
			entries[idx].occupied = true;
			entries[idx].deleted = false;
			entries[idx].value = V(std::forward<Args>(args)...);
			size_++;
		} else {
			// Overwrite or do nothing? Standard map behavior for emplace is "do nothing if exists"
			// But emplace usually takes key inside args... 
			// Let's assume emplace(key, value_args...)
			// If occupied, we don't construct new value.
		}
	}

	// contains is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool contains(const K& key) const {
		size_t idx = FindIndex(key);
		return entries[idx].occupied;
	}
	
	// find is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	V* find(const K& key) {
		size_t idx = FindIndex(key);
		if (entries[idx].occupied) {
			return &entries[idx].value;
		}
		return nullptr;
	}

	// find is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const V* find(const K& key) const {
		size_t idx = FindIndex(key);
		if (entries[idx].occupied) {
			return &entries[idx].value;
		}
		return nullptr;
	}
	
	// Simple at()
	// at is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	V& at(const K& key) {
		V* ptr = find(key);
		if (!ptr) throw std::out_of_range("FlatMap::at");
		return *ptr;
	}
	
	// at is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	const V& at(const K& key) const {
		const V* ptr = find(key);
		if (!ptr) throw std::out_of_range("FlatMap::at");
		return *ptr;
	}

	// size is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }

private:
	// FindIndex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	size_t FindIndex(const K& key) const {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (entries.empty()) return 0;
		size_t hash = std::hash<K>{}(key);
		size_t mask = entries.size() - 1;
		size_t idx = hash & mask; // Assumes power of 2 size
		
		// If not power of 2, use modulo: idx = hash % entries.size();
		// We will enforce power of 2 in rehash.
		if ((entries.size() & mask) != 0) {
			idx = hash % entries.size(); // Fallback
		}

		size_t startIdx = idx;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (entries[idx].occupied || entries[idx].deleted) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (entries[idx].occupied && entries[idx].key == key) {
				return idx;
			}
			idx = (idx + 1) & mask; // Linear probe
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if ((entries.size() & mask) != 0) idx = (idx + 1) % entries.size();

			if (idx == startIdx) {
				// Full? Should be handled by load factor check before insertion.
				// But for find() on full map (unlikely with load factor), loop terminates.
				break;
			}
		}
		return idx;
	}

	// rehash is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void rehash(size_t newCapacity) {
		// Enforce power of 2
		size_t cap = 16;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (cap < newCapacity) cap *= 2;
		
		Container oldEntries = std::move(entries);
		entries = Container(cap, allocator);
		size_ = 0;

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& entry : oldEntries) {
			if (entry.occupied) {
				// Insert directly
				size_t idx = FindIndex(entry.key);
				entries[idx].key = std::move(entry.key);
				entries[idx].value = std::move(entry.value);
				entries[idx].occupied = true;
				entries[idx].deleted = false;
				size_++;
			}
		}
	}

	Container entries;
	Allocator allocator;
	size_t size_;
};
