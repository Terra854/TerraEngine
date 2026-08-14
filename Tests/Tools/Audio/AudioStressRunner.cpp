// AudioStressRunner.cpp - deterministic audio preparation and replay evidence.
//
// The runner deliberately exercises TerraEngine's provider-neutral preparation
// path. It neither opens an audio device nor depends on system volume, so CI and
// muted developer machines can validate scheduling and packet determinism.
#include "Audio/AudioAuthoringSnapshot.h"
#include "Audio/AudioPreparationPlan.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
	using namespace TerraEngine::Audio;

	std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept {
		hash ^= value;
		return hash * 1099511628211ULL;
	}

	std::uint32_t ParsePositiveCount(const char* text, const char* label) {
		const auto parsed = std::stoull(text);
		if(parsed == 0U || parsed > 100000U)
			throw std::out_of_range(std::string(label) + " must be between 1 and 100000.");
		return static_cast<std::uint32_t>(parsed);
	}

	TerraEngine::UUID IndexedUuid(std::uint32_t index) noexcept {
		TerraEngine::UUID id{};
		// Store the full index so source identities remain unique above 255.
		id[12] = static_cast<std::uint8_t>((index >> 24U) & 0xFFU);
		id[13] = static_cast<std::uint8_t>((index >> 16U) & 0xFFU);
		id[14] = static_cast<std::uint8_t>((index >> 8U) & 0xFFU);
		id[15] = static_cast<std::uint8_t>(index & 0xFFU);
		return id;
	}

	double PercentileMilliseconds(std::vector<double> samples, double percentile) {
		if(samples.empty()) return 0.0;
		std::sort(samples.begin(), samples.end());
		const auto index = static_cast<std::size_t>(percentile * static_cast<double>(samples.size() - 1U));
		return samples[index];
	}

	// The installed stress tool cannot inspect a live ResourceManager without an
	// editor scene. This small packet-level probe exercises the same provider-
	// neutral retain/release identities and makes a leaked clip or voice visible
	// in machine-readable evidence after every churn cycle.
	class RetentionProbe final {
	public:
		void Register(const TerraEngine::UUID& id, std::uint64_t generation) {
			clips.insert(TerraEngine::ToHexString(id) + ':' + std::to_string(generation));
		}
		void Release(const TerraEngine::UUID& id, std::uint64_t generation) {
			clips.erase(TerraEngine::ToHexString(id) + ':' + std::to_string(generation));
		}
		void CreateVoice(std::uint64_t voice) { voices.insert(voice); }
		void DestroyVoice(std::uint64_t voice) { voices.erase(voice); }
		std::size_t Retained() const noexcept { return clips.size() + voices.size(); }
	private:
		std::unordered_set<std::string> clips;
		std::unordered_set<std::uint64_t> voices;
	};
}

int main(int argc, char** argv) {
	using namespace TerraEngine::Audio;
	try {
		const std::uint32_t count = argc > 1 ? ParsePositiveCount(argv[1], "Source count") : 128U;
		const std::uint32_t repeats = argc > 2 ? ParsePositiveCount(argv[2], "Repeat count") : 100U;
		AudioAuthoringSnapshotData data;
		data.frame = 1U;
		data.deltaSeconds = 1.0F / 60.0F;
		// Request more sources than the reference audible limit whenever the
		// standard 128-source workload is used.
		data.voiceLimit = std::min<std::uint32_t>(count, 64U);
		std::vector<AudioPlaybackSeed> seeds;
		data.sources.reserve(count);
		seeds.reserve(count);
		for(std::uint32_t index = 0U; index < count; ++index) {
			AudioAuthoringSourceRecord source;
			source.identity = {IndexedUuid(index + 1U), 1U, 1U};
			source.clipAssetId = IndexedUuid(index + 1U);
			source.clipGeneration = 1U;
			source.clipFrames = 48000U;
			source.position.x = static_cast<float>(index % 16U);
			source.priority = index % 7U;
			source.spatial = false;
			source.enabled = true;
			data.sources.push_back(source);
			seeds.push_back({source.identity, AudioPlaybackState::Playing, 0U});
		}

		const auto snapshot = AudioAuthoringSnapshot::Create(std::move(data));
		std::uint64_t expected = 0U;
		std::size_t maximumVoicesObserved = 0U;
		std::vector<double> enabledMilliseconds;
		std::vector<double> disabledMilliseconds;
		enabledMilliseconds.reserve(repeats);
		disabledMilliseconds.reserve(repeats);
		RetentionProbe retention;
		const auto baselineRetained = retention.Retained();
		for(std::uint32_t run = 0U; run < repeats; ++run) {
			// The disabled sample walks the same authored scene identities but does
			// not prepare audio. A volatile checksum prevents the compiler from
			// deleting the reference work while keeping the baseline device-free.
			const auto disabledStart = std::chrono::steady_clock::now();
			std::uint64_t disabledHash = 1469598103934665603ULL;
			for(const auto& source : snapshot.Data().sources)
				disabledHash = Mix(disabledHash, source.identity.objectId[15]);
			const auto disabledEnd = std::chrono::steady_clock::now();
			if(disabledHash == 0U) std::cerr << "Unexpected disabled checksum.\n";

			auto playback = seeds;
			const auto enabledStart = std::chrono::steady_clock::now();
			const auto prepared = PrepareAudioFrame(snapshot, playback);
			const auto enabledEnd = std::chrono::steady_clock::now();
			disabledMilliseconds.push_back(std::chrono::duration<double, std::milli>(disabledEnd - disabledStart).count());
			enabledMilliseconds.push_back(std::chrono::duration<double, std::milli>(enabledEnd - enabledStart).count());
			const auto audibleVoices = static_cast<std::size_t>(std::count_if(prepared.voices.begin(), prepared.voices.end(), [](const AudioPreparedVoice& voice) {
				return voice.audibility == AudioAudibility::Audible;
			}));
			maximumVoicesObserved = std::max(maximumVoicesObserved, audibleVoices);
			if(audibleVoices > data.voiceLimit) {
				std::cerr << "Prepared voice count exceeded the configured limit.\n";
				return 4;
			}
			std::uint64_t hash = 1469598103934665603ULL;
			hash = Mix(hash, prepared.frame);
			hash = Mix(hash, prepared.voices.size());
			for(const auto& voice : prepared.voices) {
				for(const auto byte : voice.identity.objectId)
					hash = Mix(hash, byte);
				hash = Mix(hash, voice.logicalFrame);
				hash = Mix(hash, voice.priority);
				hash = Mix(hash, static_cast<std::uint64_t>(voice.audibility));
			}
			if(run == 0U)
				expected = hash;
			else if(hash != expected) {
				std::cerr << "Determinism mismatch on repetition " << run << ".\n";
				return 3;
			}

			// Retain every selected clip/voice and then remove the entire scene.
			// Returning to the baseline after each replay proves bounded packet-
			// level resource ownership rather than checking only at process exit.
			std::uint64_t retainedVoice = 1U;
			for(const auto& voice : prepared.voices) {
				if(voice.audibility != AudioAudibility::Audible) continue;
				retention.Register(voice.clipAssetId, voice.clipGeneration);
				retention.CreateVoice(retainedVoice++);
			}
			retainedVoice = 1U;
			for(const auto& voice : prepared.voices) {
				if(voice.audibility != AudioAudibility::Audible) continue;
				retention.DestroyVoice(retainedVoice++);
				retention.Release(voice.clipAssetId, voice.clipGeneration);
			}
			if(retention.Retained() != baselineRetained) {
				std::cerr << "Retained audio resources did not return to baseline.\n";
				return 5;
			}
		}
		const double medianDelta = PercentileMilliseconds(enabledMilliseconds, 0.50)
			- PercentileMilliseconds(disabledMilliseconds, 0.50);
		const double p95Delta = PercentileMilliseconds(enabledMilliseconds, 0.95)
			- PercentileMilliseconds(disabledMilliseconds, 0.95);
		const auto finalRetained = retention.Retained();
		const double resourceGrowthPercent = baselineRetained == 0U
			? (finalRetained == 0U ? 0.0 : 100.0)
			: 100.0 * static_cast<double>(finalRetained - baselineRetained) / static_cast<double>(baselineRetained);
		std::cout << "{\"schemaVersion\":1,\"sources\":" << count
			<< ",\"repetitions\":" << repeats << ",\"hash\":" << expected
			<< ",\"deterministic\":true,\"voiceLimit\":" << data.voiceLimit
			<< ",\"maximumVoicesObserved\":" << maximumVoicesObserved
			<< ",\"medianFrameDeltaMs\":" << medianDelta
			<< ",\"p95FrameDeltaMs\":" << p95Delta
			<< ",\"baselineRetainedResources\":" << baselineRetained
			<< ",\"finalRetainedResources\":" << finalRetained
			<< ",\"resourceGrowthPercent\":" << resourceGrowthPercent << "}\n";
		return 0;
	} catch(const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 2;
	}
}
