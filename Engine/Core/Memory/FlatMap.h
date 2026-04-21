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

	explicit FlatMap(size_t capacity = 16, const Allocator& alloc = Allocator())
		: entries(capacity, alloc), allocator(alloc), size_(0) {}

	// No copy constructor for now to keep it simple, or implement deep copy if needed.
	// Move constructor
	FlatMap(FlatMap&& other) noexcept = default;
	FlatMap& operator=(FlatMap&& other) noexcept = default;

	void clear() {
		for (auto& entry : entries) {
			entry.occupied = false;
			entry.deleted = false;
			// Optionally destruct value if non-trivial
		}
		size_ = 0;
	}

	void reserve(size_t capacity) {
		if (capacity > entries.size()) {
			rehash(capacity);
		}
	}

	V& operator[](const K& key) {
		if (size_ >= entries.size() * 0.7) { // Load factor 0.7
			rehash(std::max(size_t(16), entries.size() * 2));
		}

		size_t idx = FindIndex(key);
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

	bool contains(const K& key) const {
		size_t idx = FindIndex(key);
		return entries[idx].occupied;
	}
	
	V* find(const K& key) {
		size_t idx = FindIndex(key);
		if (entries[idx].occupied) {
			return &entries[idx].value;
		}
		return nullptr;
	}

	const V* find(const K& key) const {
		size_t idx = FindIndex(key);
		if (entries[idx].occupied) {
			return &entries[idx].value;
		}
		return nullptr;
	}
	
	// Simple at()
	V& at(const K& key) {
		V* ptr = find(key);
		if (!ptr) throw std::out_of_range("FlatMap::at");
		return *ptr;
	}
	
	const V& at(const K& key) const {
		const V* ptr = find(key);
		if (!ptr) throw std::out_of_range("FlatMap::at");
		return *ptr;
	}

	size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }

private:
	size_t FindIndex(const K& key) const {
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
		while (entries[idx].occupied || entries[idx].deleted) {
			if (entries[idx].occupied && entries[idx].key == key) {
				return idx;
			}
			idx = (idx + 1) & mask; // Linear probe
			if ((entries.size() & mask) != 0) idx = (idx + 1) % entries.size();

			if (idx == startIdx) {
				// Full? Should be handled by load factor check before insertion.
				// But for find() on full map (unlikely with load factor), loop terminates.
				break;
			}
		}
		return idx;
	}

	void rehash(size_t newCapacity) {
		// Enforce power of 2
		size_t cap = 16;
		while (cap < newCapacity) cap *= 2;
		
		Container oldEntries = std::move(entries);
		entries = Container(cap, allocator);
		size_ = 0;

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
