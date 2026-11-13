// A rejected decode must not reserve sample capacity from a declared count
// unsupported by actual bytes. Capacity observes the real public decoder vector;
// no allocator replacement, private hook, source-string check or OOM is needed.
#include "TestHarness.h"
#include "Profiling/ProfilerCapture.h"
#include <cstdint>
#include <sstream>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		constexpr std::size_t kSampleBytes = 8 + 8 + 1 + 4 * 3 + 8 + 8 + 4 + 4 + 2;
		std::string EncodedFrame(TestContext& context, unsigned samples) {
			Profiling::ProfilerFrameRecord frame; frame.frameIndex = 19; frame.durationMs = 16; frame.complete = true;
			for(unsigned index = 0; index < samples; ++index)
				frame.samples.push_back({index + 1U, 0, Profiling::ProfilerModuleId::CpuEngine, "BoundsScope", "main", "task", 0.0, 1.0, 0, 1, true, true});
			std::string bytes, error;
			context.Expect(Profiling::EncodeProfilerJournalFrame(frame, {}, bytes, error), "Actual V1 encoder must create bounded controls: " + error);
			return bytes;
		}
		std::size_t CountOffset(TestContext& context, const std::string& bytes) {
			context.Expect(bytes.size() >= 16 && bytes.substr(0, 4) == "FRM1", "Encoded control must expose actual V1 binary envelope.");
			std::uint64_t envelope = 0;
			for(unsigned index = 0; index < 8; ++index) envelope |= std::uint64_t(static_cast<unsigned char>(bytes[4 + index])) << (8 * index);
			context.Expect(envelope <= bytes.size() - 16, "Actual CBOR length must leave its sample count.");
			return 12 + static_cast<std::size_t>(envelope);
		}
		void StoreCount(std::string& bytes, std::size_t offset, std::uint32_t count) {
			for(unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<char>((count >> (8 * index)) & 255U);
		}
		void ExpectBoundedRejection(TestContext& context, const std::string& bytes, std::size_t countOffset, std::uint32_t declared) {
			Profiling::ProfilerFrameRecord decoded; std::string error;
			const auto capacityBefore = decoded.samples.capacity();
			const bool accepted = Profiling::DecodeProfilerJournalFrame(bytes, decoded, error);
			const auto remaining = bytes.size() - countOffset - 4;
			std::ostringstream observed;
			observed << "actual V1 decode: declared=" << declared << ", remaining=" << remaining << ", capacity=" << decoded.samples.capacity()
				<< ", sampleObjectBytes=" << sizeof(Profiling::ProfilerSample) << ", ownedCapacityBytes=" << decoded.samples.capacity() * sizeof(Profiling::ProfilerSample)
				<< ", accepted=" << accepted << ", reason=" << error;
			context.Expect(!accepted && !error.empty(), observed.str());
			// Both malformed fixtures have fewer than one complete sample. A fresh
			// vector must retain its original capacity; vector growth policy cannot
			// excuse reserving even one row before the fixed record is validated.
			context.Expect(remaining < kSampleBytes && decoded.samples.capacity() == capacityBefore, observed.str());
		}
	}
	void RegisterJournalAllocationBoundsTests(TestRegistry& registry) {
		registry.Add("Engine.JournalAllocationBounds.TinyPayloadHugeDeclaredCount", [](TestContext& context) {
			auto bytes = EncodedFrame(context, 0); const auto offset = CountOffset(context, bytes);
			// 65,536 is huge relative to zero sample bytes, yet keeps the original
			// reserve safely bounded (report sizeof/capacity, not an unbounded attack).
			constexpr std::uint32_t declared = 65536; StoreCount(bytes, offset, declared); bytes.resize(offset + 4);
			ExpectBoundedRejection(context, bytes, offset, declared);
		});
		registry.Add("Engine.JournalAllocationBounds.TruncatedFixedSampleRejectsBeforeReserve", [](TestContext& context) {
			auto bytes = EncodedFrame(context, 1); const auto offset = CountOffset(context, bytes);
			context.Expect(bytes.size() - offset - 4 == kSampleBytes, "Production encoder must retain fixed55-byte V1 sample contract.");
			bytes.pop_back(); ExpectBoundedRejection(context, bytes, offset, 1);
		});
		registry.Add("Engine.JournalAllocationBounds.ExistingCountCapRejectsBeforeReserve", [](TestContext& context) {
			auto bytes = EncodedFrame(context, 0); const auto offset = CountOffset(context, bytes);
			constexpr std::uint32_t declared = (1U << 20U) + 1U; StoreCount(bytes, offset, declared);
			ExpectBoundedRejection(context, bytes, offset, declared);
		});
		registry.Add("Engine.JournalAllocationBounds.ValidRowsRoundTripAndTrailingBytesReject", [](TestContext& context) {
			for(const auto count : {0U, 1U, 3U}) {
				auto bytes = EncodedFrame(context, count); Profiling::ProfilerFrameRecord decoded; std::string error;
				context.Expect(Profiling::DecodeProfilerJournalFrame(bytes, decoded, error), "Actual valid V1 payload must remain accepted: " + error);
				context.Expect(decoded.frameIndex == 19 && decoded.samples.size() == count, "Valid frame/sample counts must round-trip.");
				for(unsigned index = 0; index < count; ++index) {
					const auto& sample = decoded.samples[index];
					context.Expect(sample.sampleId == index + 1U && sample.parentSampleId == 0 && sample.moduleId == Profiling::ProfilerModuleId::CpuEngine
						&& sample.name == "BoundsScope" && sample.threadLabel == "main" && sample.taskLabel == "task"
						&& sample.startMs == 0 && sample.durationMs == 1 && sample.depth == 0 && sample.occurrenceCount == 1 && sample.complete && sample.retainInAggregate,
						"Valid sample fields must survive the unchanged binary format.");
				}
				bytes.push_back('x'); Profiling::ProfilerFrameRecord trailing;
				context.Expect(!Profiling::DecodeProfilerJournalFrame(bytes, trailing, error), "Trailing sample bytes must remain rejected.");
			}
		});
	}
}
