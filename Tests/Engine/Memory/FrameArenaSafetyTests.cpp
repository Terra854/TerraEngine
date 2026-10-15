// FrameArenaSafetyTests.cpp - Address, range, and failure contracts for retained allocators.
#include "TestHarness.h"

#include "Core/Memory/FrameArena.h"
#include "Core/Memory/ReusableArenaResource.h"
#include "Core/Memory/TrackingMemoryResource.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace TerraEngine::Memory::Detail {
	// A friend peer observes existing state without adding a singleton public API.
	struct FrameArenaTestAccess {
		struct State { std::size_t position; std::size_t fallbackBlocks; };
		static State Inspect(FrameArena& arena) { return { arena.offset, arena.fallbackBlocks.size() }; }
		static std::size_t Capacity(FrameArena& arena) { return arena.bufferSize; }
		static void Configure(FrameArena& arena, void (*allocate)(void*), void (*track)(void*), void* context) {
			arena.fallbackAllocationCheckpoint_ = allocate;
			arena.fallbackTrackingCheckpoint_ = track;
			arena.fallbackCheckpointContext_ = context;
		}
	};
}

namespace TerraEngine::Tests {
	namespace {
		// Singleton tests run serially in the harness. Every episode retires its raw
		// borrowers before this guard releases fallback ownership and resets backing.
		struct FrameArenaEpisode {
			FrameArena& arena = FrameArena::Instance();
			FrameArenaEpisode() { arena.Reset(); }
			~FrameArenaEpisode() { arena.Reset(); }
		};

		bool IsAligned(const void* address, std::size_t alignment) {
			return address != nullptr && reinterpret_cast<std::uintptr_t>(address) % alignment == 0U;
		}

		// Failure affects only this test-owned upstream resource. It never replaces
		// global allocation, and successful fallback ownership remains countable.
		struct ControlledUpstream final : std::pmr::memory_resource {
			bool fail = false;
			std::size_t allocations = 0U;
			std::size_t deallocations = 0U;
			void* do_allocate(std::size_t bytes, std::size_t alignment) override {
				if (fail) throw std::bad_alloc();
				void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
				++allocations;
				return result;
			}
			void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
				std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
				++deallocations;
			}
			bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
		};

		struct UnexpectedFallbackAttempt {};
		struct FrameFaultState {
			std::size_t allocationAttempts = 0U;
			std::size_t trackingAttempts = 0U;
			bool forbidAllocation = false;
			bool failAllocation = false;
			bool failTracking = false;
		};
		struct FrameFaultScope {
			FrameArena& arena;
			FrameFaultState state;
			explicit FrameFaultScope(FrameArena& value) : arena(value) {
				Memory::Detail::FrameArenaTestAccess::Configure(arena, [](void* raw) {
					auto& fault = *static_cast<FrameFaultState*>(raw);
					++fault.allocationAttempts;
					if (fault.forbidAllocation) throw UnexpectedFallbackAttempt{};
					if (fault.failAllocation) throw std::bad_alloc();
				}, [](void* raw) {
					auto& fault = *static_cast<FrameFaultState*>(raw);
					++fault.trackingAttempts;
					if (fault.failTracking) throw std::bad_alloc();
				}, &state);
			}
			~FrameFaultScope() { Memory::Detail::FrameArenaTestAccess::Configure(arena, nullptr, nullptr, nullptr); }
		};

		template<typename Exception, typename Action>
		bool ThrowsAllocationType(Action&& action) {
			try { action(); }
			catch (const Exception&) { return true; }
			catch (...) { return false; }
			return false;
		}

		void RequireSentinelAndState(TestContext& context, FrameArena& arena,
			Memory::Detail::FrameArenaTestAccess::State before, const unsigned char* sentinel) {
			const auto after = Memory::Detail::FrameArenaTestAccess::Inspect(arena);
			context.Expect(after.position == before.position && after.fallbackBlocks == before.fallbackBlocks,
				"rejected requests must preserve backing position and accepted fallback ownership");
			for (std::size_t index = 0U; index < 64U; ++index)
				context.Expect(sentinel[index] == 0x5aU, "rejected requests must preserve every sentinel byte");
		}

		void ObserveOverflow(const char* caseId, FrameArena& arena, std::size_t rejectionCount,
			std::size_t allocationAttempts = 0U, std::size_t trackingAttempts = 0U, std::size_t sentinelBytesChecked = 64U) {
			const auto state = Memory::Detail::FrameArenaTestAccess::Inspect(arena);
			std::cout << "RMS_OBSERVATION {\"caseId\":\"" << caseId << "\",\"position\":" << state.position
				<< ",\"fallbackBlocks\":" << state.fallbackBlocks << ",\"rejectionCount\":" << rejectionCount
				<< ",\"allocationAttempts\":" << allocationAttempts << ",\"trackingAttempts\":" << trackingAttempts
				<< ",\"sentinelBytesChecked\":" << sentinelBytesChecked << "}\n";
		}
	}

	void RegisterFrameArenaSafetyTests(TestRegistry& registry) {
		using namespace Memory;
		registry.Add("Engine.RuntimeMemorySafety.Allocators.Alignment.ControlledBacking64And256", [](TestContext& context) {
			alignas(256) std::array<std::uint8_t, 1024U> backing{};
			// Starting one byte after a known aligned address defeats allocator luck.
			// Both requests start at position zero and call the singleton's fast path.
			std::size_t offset64 = 0U;
			std::size_t offset256 = 0U;
			auto* start = backing.data() + 1U;
			void* result64 = Detail::TryAllocateFrameArenaSpan(start, backing.size() - 1U, offset64, 32U, 64U);
			void* result256 = Detail::TryAllocateFrameArenaSpan(start, backing.size() - 1U, offset256, 32U, 256U);
			std::cout << "controlled backing=" << static_cast<void*>(start)
				<< " alignment64=" << result64 << " alignment256=" << result256 << '\n';
			context.Expect(IsAligned(result64, 64U) && IsAligned(result256, 256U),
				"actual backing addresses must align at both 64 and 256 bytes from a deliberately misaligned base");
			context.Expect(offset64 <= backing.size() - 1U && offset256 <= backing.size() - 1U,
				"padding and payload must remain inside the controlled backing span");
			FrameArenaEpisode episode;
			context.Expect(IsAligned(episode.arena.Allocate(8U, 8U), 8U),
				"ordinary singleton allocation must remain supported");
			std::cout << "RMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.Allocators.Alignment.ControlledBacking64And256\",\"backing\":\""
				<< static_cast<void*>(start) << "\",\"address64\":\"" << result64 << "\",\"address256\":\"" << result256
				<< "\",\"position64\":" << offset64 << ",\"position256\":" << offset256 << "}\n";
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Alignment.MixedLiveRanges", [](TestContext& context) {
			FrameArenaEpisode episode;
			std::array<void*, 3U> addresses{};
			const std::array<std::size_t, 3U> alignments{ 8U, 64U, 256U };
			for (std::size_t index = 0U; index < addresses.size(); ++index) {
				addresses[index] = episode.arena.Allocate(37U, alignments[index]);
				context.Expect(IsAligned(addresses[index], alignments[index]), "mixed requests must align actual live addresses");
				std::memset(addresses[index], static_cast<int>(index + 1U), 37U);
			}
			for (std::size_t index = 0U; index < addresses.size(); ++index) {
				const auto* bytes = static_cast<const unsigned char*>(addresses[index]);
				for (std::size_t byte = 0U; byte < 37U; ++byte)
					context.Expect(bytes[byte] == index + 1U, "later allocation must not overlap an earlier live range");
			}
			ReusableArenaResource resource(2048U);
			for (std::size_t alignment : alignments)
				context.Expect(IsAligned(resource.allocate(37U, alignment), alignment), "reusable fixed backing must satisfy the same alignments");
			context.Expect(resource.Statistics().fallbackAllocationCount == 0U, "small reusable allocations must use fixed backing");
			std::cout << "RMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.Allocators.Alignment.MixedLiveRanges\",\"address8\":\""
				<< addresses[0] << "\",\"address64\":\"" << addresses[1] << "\",\"address256\":\"" << addresses[2]
				<< "\",\"sentinelBytesChecked\":111,\"reusableUsedBytes\":" << resource.Statistics().usedBytes
				<< ",\"reusableFallbackCount\":" << resource.Statistics().fallbackAllocationCount << "}\n";
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Alignment.AlignedFallbackAndUpstream", [](TestContext& context) {
			FrameArenaEpisode episode;
			std::array<void*, 2U> fallbackAddresses{};
			std::size_t index = 0U;
			for (std::size_t alignment : {64U, 256U}) {
				void* result = episode.arena.Allocate(17U * 1024U * 1024U, alignment);
				fallbackAddresses[index++] = result;
				context.Expect(IsAligned(result, alignment), "oversized standalone fallback must meet requested alignment");
				static_cast<unsigned char*>(result)[17U * 1024U * 1024U - 1U] = 73U;
			}
			TrackingMemoryResource upstream;
			{
				ReusableArenaResource resource(16U, &upstream);
				for (std::size_t alignment : {64U, 256U})
					context.Expect(IsAligned(resource.allocate(512U, alignment), alignment), "PMR fallback must use aligned upstream allocation");
				context.Expect(resource.Statistics().fallbackAllocationCount == 2U
					&& upstream.Statistics().allocationCount == 2U, "all fallback pressure must be reported");
				resource.Reset();
				context.Expect(upstream.Statistics().bytesInUse == 0U, "reset must release aligned upstream ownership");
			}
			context.Expect(upstream.Statistics().allocationCount == upstream.Statistics().deallocationCount,
				"fallback cleanup must balance each upstream allocation exactly once");
			ControlledUpstream controlled;
			ReusableArenaResource resource(16U, &controlled);
			controlled.fail = true;
			bool rejected = false;
			try { (void)resource.allocate(512U, 256U); } catch (const std::bad_alloc&) { rejected = true; }
			context.Expect(rejected && resource.Statistics().usedBytes == 0U
				&& resource.Statistics().fallbackAllocationCount == 0U && controlled.allocations == 0U,
				"upstream failure must leave fixed position and fallback acceptance unchanged");
			controlled.fail = false;
			context.Expect(IsAligned(resource.allocate(512U, 256U), 256U), "healthy upstream work must remain usable after failure");
			resource.Reset();
			context.Expect(controlled.allocations == controlled.deallocations, "retry fallback ownership must balance after reset");
			std::cout << "RMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.Allocators.Alignment.AlignedFallbackAndUpstream\",\"standaloneAddress64\":\""
				<< fallbackAddresses[0] << "\",\"standaloneAddress256\":\"" << fallbackAddresses[1]
				<< "\",\"upstreamAllocations\":" << upstream.Statistics().allocationCount
				<< ",\"upstreamDeallocations\":" << upstream.Statistics().deallocationCount
				<< ",\"retryAllocations\":" << controlled.allocations << ",\"retryDeallocations\":" << controlled.deallocations << "}\n";
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Alignment.ResetReusesRetiredBacking", [](TestContext& context) {
			FrameArenaEpisode episode;
			void* first = episode.arena.Allocate(93U, 256U);
			context.Expect(IsAligned(first, 256U), "first backing episode must satisfy over-alignment");
			episode.arena.Reset();
			void* second = episode.arena.Allocate(93U, 256U);
			context.Expect(second == first && IsAligned(second, 256U), "reset must reuse stable backing after borrowers retire");
			ReusableArenaResource resource(1024U);
			void* reusableFirst = resource.allocate(93U, 256U);
			resource.Reset();
			context.Expect(resource.allocate(93U, 256U) == reusableFirst
				&& resource.Statistics().resetCount == 1U, "reusable backing must also retain capacity across episodes");
			std::cout << "RMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.Allocators.Alignment.ResetReusesRetiredBacking\",\"first\":\""
				<< first << "\",\"afterReset\":\"" << second << "\",\"reusableAddress\":\"" << reusableFirst
				<< "\",\"reusableResetCount\":" << resource.Statistics().resetCount
				<< ",\"reusableUsedBytes\":" << resource.Statistics().usedBytes << "}\n";
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Overflow.DirectBytesRejectWrap", [](TestContext& context) {
			FrameArenaEpisode episode;
			auto* sentinel = static_cast<unsigned char*>(episode.arena.Allocate(64U, 1U));
			std::memset(sentinel, 0x5a, 64U);
			const auto before = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			FrameFaultScope faults(episode.arena);
			faults.state.forbidAllocation = true;
			for (std::size_t alignment : {1U, 8U, 256U}) {
				context.Expect(ThrowsAllocationType<std::bad_alloc>([&] {
					(void)episode.arena.Allocate((std::numeric_limits<std::size_t>::max)(), alignment);
				}), "unrepresentable direct bytes must reject before attempting backing allocation");
			}
			context.Expect(ThrowsAllocationType<std::bad_alloc>([&] {
				(void)episode.arena.Allocate(1U, (std::numeric_limits<std::size_t>::max)() / 2U + 1U);
			}), "an unrepresentable fallback alignment span must reject before allocation");
			RequireSentinelAndState(context, episode.arena, before, sentinel);
			context.Expect(faults.state.allocationAttempts == 0U, "direct wrap must be checked before allocation checkpoint");
			ObserveOverflow("Engine.RuntimeMemorySafety.Allocators.Overflow.DirectBytesRejectWrap", episode.arena, 4U,
				faults.state.allocationAttempts, faults.state.trackingAttempts);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Overflow.NewArrayRejectsCountOverflow", [](TestContext& context) {
			FrameArenaEpisode episode;
			auto* sentinel = static_cast<unsigned char*>(episode.arena.Allocate(64U, 1U));
			std::memset(sentinel, 0x5a, 64U);
			const auto before = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			context.Expect(ThrowsAllocationType<std::bad_array_new_length>([&] {
				(void)episode.arena.NewArray<std::uint64_t>(std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) + 1U);
			}), "NewArray multiplication overflow must throw bad_array_new_length before acceptance");
			RequireSentinelAndState(context, episode.arena, before, sentinel);
			std::uint64_t* valid = episode.arena.NewArray<std::uint64_t>(4U);
			context.Expect(IsAligned(valid, alignof(std::uint64_t)), "valid array storage must remain usable after count rejection");
			std::memset(valid, 0x27, 4U * sizeof(std::uint64_t));
			for (std::size_t index = 0U; index < 64U; ++index)
				context.Expect(sentinel[index] == 0x5aU, "healthy array storage must not overlap the sentinel");
			ObserveOverflow("Engine.RuntimeMemorySafety.Allocators.Overflow.NewArrayRejectsCountOverflow", episode.arena, 1U);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Overflow.FrameAllocatorRejectsCountOverflow", [](TestContext& context) {
			FrameArenaEpisode episode;
			auto* sentinel = static_cast<unsigned char*>(episode.arena.Allocate(64U, 1U));
			std::memset(sentinel, 0x5a, 64U);
			const auto before = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			FrameAllocator<std::uint64_t> allocator;
			context.Expect(ThrowsAllocationType<std::bad_array_new_length>([&] {
				(void)allocator.allocate(std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) + 1U);
			}), "allocator wrapper count overflow must throw bad_array_new_length before acceptance");
			RequireSentinelAndState(context, episode.arena, before, sentinel);
			std::uint64_t* valid = allocator.allocate(4U);
			context.Expect(IsAligned(valid, alignof(std::uint64_t)), "valid allocator storage must remain usable after count rejection");
			std::memset(valid, 0x27, 4U * sizeof(std::uint64_t));
			allocator.deallocate(valid, 4U);
			for (std::size_t index = 0U; index < 64U; ++index)
				context.Expect(sentinel[index] == 0x5aU, "healthy allocator storage must not overlap the sentinel");
			ObserveOverflow("Engine.RuntimeMemorySafety.Allocators.Overflow.FrameAllocatorRejectsCountOverflow", episode.arena, 1U);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Overflow.FallbackAdjustedSizeRejectsWrap", [](TestContext& context) {
			FrameArenaEpisode episode;
			auto* sentinel = static_cast<unsigned char*>(episode.arena.Allocate(64U, 1U));
			std::memset(sentinel, 0x5a, 64U);
			const auto before = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			FrameFaultScope faults(episode.arena);
			faults.state.forbidAllocation = true;
			context.Expect(ThrowsAllocationType<std::bad_alloc>([&] {
				(void)episode.arena.Allocate(std::numeric_limits<std::size_t>::max() - 127U, 256U);
			}), "fallback padding adjustment must reject overflow before allocation");
			RequireSentinelAndState(context, episode.arena, before, sentinel);
			context.Expect(faults.state.allocationAttempts == 0U, "adjusted fallback wrap must never attempt upstream allocation");
			ObserveOverflow("Engine.RuntimeMemorySafety.Allocators.Overflow.FallbackAdjustedSizeRejectsWrap", episode.arena, 1U,
				faults.state.allocationAttempts, faults.state.trackingAttempts);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Overflow.ExactFitAndRejectedRequestPreserveState", [](TestContext& context) {
			alignas(256) std::array<std::uint8_t, 256U> backing{};
			std::size_t position = 0U;
			void* exact = Detail::TryAllocateFrameArenaSpan(backing.data(), backing.size(), position, backing.size(), 1U);
			context.Expect(exact == backing.data() && position == backing.size(), "exact capacity must remain supported");
			backing.front() = 19U;
			backing.back() = 71U;
			context.Expect(Detail::TryAllocateFrameArenaSpan(backing.data(), backing.size(), position, 1U, 1U) == nullptr
				&& position == backing.size() && backing.front() == 19U && backing.back() == 71U,
				"full-span rejection must preserve position and live edge sentinels");
			FrameArenaEpisode episode;
			const auto capacity = Detail::FrameArenaTestAccess::Capacity(episode.arena);
			auto* whole = static_cast<unsigned char*>(episode.arena.Allocate(capacity, 1U));
			whole[0] = 19U;
			whole[capacity - 1U] = 71U;
			void* fallback = episode.arena.Allocate(32U, 64U);
			context.Expect(IsAligned(fallback, 64U) && whole[0] == 19U && whole[capacity - 1U] == 71U,
				"healthy fallback after exact fit must preserve live backing");
			ObserveOverflow("Engine.RuntimeMemorySafety.Allocators.Overflow.ExactFitAndRejectedRequestPreserveState", episode.arena, 1U, 0U, 0U, 4U);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.Overflow.ZeroInvalidAndFaultPolicies", [](TestContext& context) {
			FrameArenaEpisode episode;
			const auto before = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			context.Expect(episode.arena.Allocate(0U, 8U) == nullptr, "zero bytes must return null without advancing state");
			context.Expect(episode.arena.NewArray<std::uint64_t>(0U) == nullptr
				&& FrameAllocator<std::uint64_t>{}.allocate(0U) == nullptr,
				"both retained count-based interfaces must follow the null zero policy");
			// The production entry point now validates before invoking std::align;
			// exercise invalid alignments for both empty and nonempty requests.
			for (std::size_t alignment : {0U, 3U, 6U}) {
				for (std::size_t bytes : {0U, 16U}) {
					context.Expect(ThrowsAllocationType<std::invalid_argument>([&] {
						(void)episode.arena.Allocate(bytes, alignment);
					}), "invalid alignment must reject before state changes or std::align");
				}
			}
			const auto after = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			context.Expect(before.position == after.position && before.fallbackBlocks == after.fallbackBlocks,
				"zero requests must not change position or own fallback blocks");
			auto* sentinel = static_cast<unsigned char*>(episode.arena.Allocate(64U, 1U));
			std::memset(sentinel, 0x5a, 64U);
			const auto fallbackBytes = Detail::FrameArenaTestAccess::Capacity(episode.arena) + 1U;
			auto* existingFallback = static_cast<unsigned char*>(episode.arena.Allocate(fallbackBytes, 64U));
			std::memset(existingFallback, 0x39, 32U);
			const auto occupied = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			FrameFaultScope faults(episode.arena);
			faults.state.failAllocation = true;
			context.Expect(ThrowsAllocationType<std::bad_alloc>([&] { (void)episode.arena.Allocate(fallbackBytes, 64U); }),
				"scoped upstream checkpoint failure must propagate");
			RequireSentinelAndState(context, episode.arena, occupied, sentinel);
			faults.state.failAllocation = false;
			faults.state.failTracking = true;
			context.Expect(ThrowsAllocationType<std::bad_alloc>([&] { (void)episode.arena.Allocate(fallbackBytes, 256U); }),
				"scoped ownership tracking failure must propagate after temporary backing allocation");
			RequireSentinelAndState(context, episode.arena, occupied, sentinel);
			faults.state.failTracking = false;
			context.Expect(IsAligned(episode.arena.Allocate(fallbackBytes, 256U), 256U), "healthy fallback must remain usable after failures");
			for (std::size_t index = 0U; index < 32U; ++index)
				context.Expect(existingFallback[index] == 0x39U, "failed allocation and tracking must preserve earlier fallback storage");
			const auto recovered = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			context.Expect(recovered.fallbackBlocks == occupied.fallbackBlocks + 1U,
				"only the healthy retry may add accepted fallback ownership");
			const auto beforeZero = recovered;
			context.Expect(episode.arena.Allocate(0U, 256U) == nullptr, "zero requests must not allocate while fallback ownership is live");
			RequireSentinelAndState(context, episode.arena, beforeZero, sentinel);
			episode.arena.Reset();
			const auto retired = Detail::FrameArenaTestAccess::Inspect(episode.arena);
			context.Expect(retired.position == 0U && retired.fallbackBlocks == 0U,
				"reset after borrowers retire must reclaim every accepted fallback and backing position");
			std::cout << "RMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.Allocators.Overflow.ZeroInvalidAndFaultPolicies\",\"position\":"
				<< retired.position << ",\"fallbackBlocks\":" << retired.fallbackBlocks
				<< ",\"positionBeforeReset\":" << recovered.position << ",\"fallbackBlocksBeforeReset\":" << recovered.fallbackBlocks
				<< ",\"rejectionCount\":8,\"allocationAttempts\":" << faults.state.allocationAttempts
				<< ",\"trackingAttempts\":" << faults.state.trackingAttempts << ",\"sentinelBytesChecked\":96}\n";
		});
	}
}
