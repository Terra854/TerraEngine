// MiniaudioBackend.cpp - concrete device/decoder implementation behind IAudioBackend.

#include "Audio/Backend/MiniaudioBackend.h"
#include "Audio/Backend/AudioBackendRegistry.h"

// This is the sole TerraEngine source which includes the third-party SDK.
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Audio::Backend {
	namespace {
		constexpr std::size_t kMaximumObservations = 128U;

		std::string ClipKey(const TerraEngine::UUID& id, AudioGeneration generation) {
			return TerraEngine::ToHexString(id) + ':' + std::to_string(generation);
		}

		enum class CallbackObservation : std::uint8_t {
			DeviceLost,
		};

		struct MiniaudioVoice final {
			ma_decoder decoder{};
			ma_sound sound{};
			std::shared_ptr<const std::vector<std::uint8_t>> payload;
			AudioSourceIdentity identity;
			bool decoderInitialized = false;
			bool soundInitialized = false;
			// The provider remembers whether output was virtualized/paused so the
			// next audible packet can resynchronize to the engine logical cursor.
			bool wasPaused = true;

			~MiniaudioVoice() {
				if(soundInitialized) ma_sound_uninit(&sound);
				if(decoderInitialized) ma_decoder_uninit(&decoder);
			}
		};
	}

	struct MiniaudioBackend::Impl {
		ma_engine engine{};
		ma_device device{};
		bool engineInitialized = false;
		bool deviceInitialized = false;
		bool initialized = false;
		AudioPacketSequence lastSequence = 0U;
		AudioBackendCreateInfo createInfo;
		std::unordered_map<AudioVoiceToken, std::unique_ptr<MiniaudioVoice>> voices;
		std::unordered_map<std::string, AudioClipRegistration> clips;

		// The miniaudio notification callback can run on a device-owned thread.
		// It is therefore limited to this preallocated, non-blocking event channel:
		// no strings, containers, allocation, SDK restart, or engine locks occur
		// there. PollObservations translates the compact records later at a safe
		// protected boundary.
		std::array<std::atomic<std::uint8_t>, kMaximumObservations> callbackObservations{};
		std::atomic<std::uint64_t> callbackRead{0U};
		std::atomic<std::uint64_t> callbackWrite{0U};
		std::atomic<std::uint64_t> callbackDrops{0U};
		std::atomic_flag callbackProducer = ATOMIC_FLAG_INIT;
		std::atomic<bool> acceptsCallbacks{false};
		std::atomic<bool> deviceLost{false};
		bool recoveryPublicationPending = false;
		std::uint64_t pendingCallbackDrops = 0U;

		// Voice completion originates from SubmitFrame's protected caller rather
		// than the callback. This fixed ring keeps the public observation contract
		// bounded for both sources without relying on a deque.
		std::array<AudioBackendObservation, kMaximumObservations> providerObservations{};
		std::size_t providerRead = 0U;
		std::size_t providerWrite = 0U;
		std::size_t providerCount = 0U;
		std::uint64_t providerDrops = 0U;

		void PushCallbackObservation(CallbackObservation observation) noexcept {
			if(!acceptsCallbacks.load(std::memory_order_acquire)) return;
			// A notification can arrive from a platform callback while another
			// notification is unwinding. Dropping the newer record is safer than
			// waiting on an audio thread; the overflow diagnostic makes that visible.
			if(callbackProducer.test_and_set(std::memory_order_acquire)) {
				callbackDrops.fetch_add(1U, std::memory_order_relaxed);
				return;
			}
			const std::uint64_t write = callbackWrite.load(std::memory_order_relaxed);
			const std::uint64_t read = callbackRead.load(std::memory_order_acquire);
			if(write - read >= kMaximumObservations) {
				callbackDrops.fetch_add(1U, std::memory_order_relaxed);
			} else {
				callbackObservations[write % kMaximumObservations].store(
					static_cast<std::uint8_t>(observation), std::memory_order_relaxed);
				callbackWrite.store(write + 1U, std::memory_order_release);
			}
			callbackProducer.clear(std::memory_order_release);
		}

		void MarkDeviceLost() noexcept {
			// Coalescing repeated stop/interruption notifications means one recovery
			// request always represents one complete logical reconciliation episode.
			if(!deviceLost.exchange(true, std::memory_order_acq_rel))
				PushCallbackObservation(CallbackObservation::DeviceLost);
		}

		void PushProviderObservation(AudioBackendObservation observation) {
			if(providerCount < kMaximumObservations) {
				providerObservations[providerWrite] = std::move(observation);
				providerWrite = (providerWrite + 1U) % kMaximumObservations;
				++providerCount;
			} else {
				++providerDrops;
			}
		}

		static void DeviceDataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
			// The engine is fully initialized before the device is started. Reading
			// directly into miniaudio's output buffer performs no TerraEngine work.
			auto* owner = static_cast<Impl*>(device->pUserData);
			if(owner != nullptr && owner->engineInitialized)
				(void)ma_engine_read_pcm_frames(&owner->engine, output, frameCount, nullptr);
		}

		static void DeviceNotificationCallback(const ma_device_notification* notification) {
			if(notification == nullptr || notification->pDevice == nullptr) return;
			auto* owner = static_cast<Impl*>(notification->pDevice->pUserData);
			if(owner == nullptr) return;
			switch(notification->type) {
			case ma_device_notification_type_stopped:
			case ma_device_notification_type_rerouted:
			case ma_device_notification_type_interruption_began:
				owner->MarkDeviceLost();
				break;
			default:
				// Restarting or uninitializing in this callback can deadlock miniaudio.
				// SubmitFrame performs any recovery after the protected frame boundary.
				break;
			}
		}
	};

	MiniaudioBackend::MiniaudioBackend() : impl(std::make_unique<Impl>()) {}
	MiniaudioBackend::~MiniaudioBackend() { (void)Shutdown(); }

	AudioResult MiniaudioBackend::Initialize(const AudioBackendCreateInfo& createInfo) {
		if(impl->initialized) return AudioResult::Accepted("miniaudio was already initialized.");
		if(createInfo.sampleRate == 0U || createInfo.channels == 0U || createInfo.maximumVoices == 0U)
			return AudioResult::Rejected("Audio device preferences must be non-zero.");

		// TerraEngine owns the device so its callback can point at the private Impl.
		// That lets the callback publish a fixed event record without a global map or
		// an allocation, while miniaudio's engine remains responsible for mixing.
		ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
		deviceConfig.playback.format = ma_format_f32;
		deviceConfig.playback.channels = createInfo.channels;
		deviceConfig.sampleRate = createInfo.sampleRate;
		deviceConfig.dataCallback = &Impl::DeviceDataCallback;
		deviceConfig.notificationCallback = &Impl::DeviceNotificationCallback;
		deviceConfig.pUserData = impl.get();
		deviceConfig.noPreSilencedOutputBuffer = MA_TRUE;
		deviceConfig.noClip = MA_TRUE;
		if(ma_device_init(nullptr, &deviceConfig, &impl->device) != MA_SUCCESS)
			return {AudioResultCode::DeviceUnavailable, "miniaudio could not open the default playback device; audio remains safely silent."};
		impl->deviceInitialized = true;

		ma_engine_config engineConfig = ma_engine_config_init();
		engineConfig.pDevice = &impl->device;
		if(ma_engine_init(&engineConfig, &impl->engine) != MA_SUCCESS) {
			ma_device_uninit(&impl->device);
			impl->deviceInitialized = false;
			return {AudioResultCode::DeviceUnavailable, "miniaudio could not initialize the playback mixer; audio remains safely silent."};
		}
		impl->engineInitialized = true;
		if(ma_device_start(&impl->device) != MA_SUCCESS) {
			ma_engine_uninit(&impl->engine);
			ma_device_uninit(&impl->device);
			impl->engineInitialized = false;
			impl->deviceInitialized = false;
			return {AudioResultCode::DeviceUnavailable, "miniaudio could not start the default playback device; audio remains safely silent."};
		}

		impl->createInfo = createInfo;
		impl->acceptsCallbacks.store(true, std::memory_order_release);
		impl->initialized = true;
		return AudioResult::Accepted();
	}

	AudioResult MiniaudioBackend::SubmitFrame(const AudioBackendFramePacket& packet) {
		if(!impl->initialized) return {AudioResultCode::InvalidState, "miniaudio is not initialized."};
		if(packet.sequence == 0U || packet.sequence != impl->lastSequence + 1U)
			return {AudioResultCode::StalePacket, "Audio packets must be strictly consecutive."};

		// Some platforms cannot emit an unplug notification. Polling miniaudio's
		// atomic device state at the protected boundary supplies that fallback.
		if(ma_device_get_state(&impl->device) != ma_device_state_started)
			impl->MarkDeviceLost();
		if(impl->deviceLost.load(std::memory_order_acquire) || impl->recoveryPublicationPending) {
			if(!packet.requestRecovery || !impl->createInfo.requestDeviceRecovery)
				return {AudioResultCode::DeviceUnavailable, "The playback device is unavailable until the engine requests reconciliation."};
			if(ma_device_get_state(&impl->device) != ma_device_state_started
				&& ma_device_start(&impl->device) != MA_SUCCESS)
				return {AudioResultCode::DeviceUnavailable, "miniaudio could not restart the playback device; audio remains safely silent."};
			// The engine packet contains every audible voice. Marking retained voices
			// paused makes their command seek to the authoritative logical cursor,
			// completing reconciliation without exposing SDK state to the engine.
			for(auto& [token, voice] : impl->voices) voice->wasPaused = true;
			impl->deviceLost.store(false, std::memory_order_release);
			// Do not report success until the complete packet is applied. If packet
			// validation fails, the next retry remains a reconciliation request.
			impl->recoveryPublicationPending = true;
		}

		// Validate the complete packet before changing device or provider state.
		// Destroy/create transitions can contain more commands than the final voice
		// count, so capacity is measured from the predicted terminal identity set.
		std::unordered_set<std::string> availableClips;
		for(const auto& [key, registration] : impl->clips) availableClips.insert(key);
		for(const auto& registration : packet.registrations) {
			if(!registration.IsValid()) return AudioResult::Rejected("A clip registration is invalid.");
			availableClips.insert(ClipKey(registration.assetId, registration.generation));
		}
		for(const auto& release : packet.releases) availableClips.erase(ClipKey(release.assetId, release.generation));
		std::unordered_set<AudioVoiceToken> predictedVoices;
		for(const auto& [token, voice] : impl->voices) predictedVoices.insert(token);
		for(const auto& command : packet.voices) {
			if(command.voice == 0U || !command.identity.IsValid())
				return AudioResult::Rejected("An audio voice command has an invalid identity.");
			if(command.destroy) predictedVoices.erase(command.voice);
			else {
				if(!availableClips.contains(ClipKey(command.clipAssetId, command.clipGeneration)))
					return AudioResult::Rejected("An audio voice command references an unavailable clip generation.");
				predictedVoices.insert(command.voice);
			}
		}
		if(predictedVoices.size() > impl->createInfo.maximumVoices)
			return {AudioResultCode::CapacityExceeded, "The packet would exceed the configured voice limit."};

		// Decoder and sound creation can fail even after structural preflight. Build
		// every newly required voice first and keep it detached from the provider
		// maps; RAII tears the staged set down if any later creation fails, leaving
		// the previously committed clips, voices, gain, and packet sequence intact.
		std::vector<std::unique_ptr<MiniaudioVoice>> stagedVoices(packet.voices.size());
		std::unordered_set<AudioVoiceToken> simulatedVoices;
		for(const auto& [token, voice] : impl->voices) simulatedVoices.insert(token);
		for(std::size_t index = 0U; index < packet.voices.size(); ++index) {
			const auto& command = packet.voices[index];
			if(command.destroy) { simulatedVoices.erase(command.voice); continue; }
			if(!simulatedVoices.insert(command.voice).second) continue;

			const std::string clipKey = ClipKey(command.clipAssetId, command.clipGeneration);
			const AudioClipRegistration* clip = nullptr;
			for(auto registration = packet.registrations.rbegin(); registration != packet.registrations.rend(); ++registration) {
				if(ClipKey(registration->assetId, registration->generation) == clipKey) { clip = &*registration; break; }
			}
			if(clip == nullptr) {
				const auto existing = impl->clips.find(clipKey);
				if(existing != impl->clips.end()) clip = &existing->second;
			}
			if(clip == nullptr)
				return AudioResult::Rejected("A staged voice references an unavailable clip generation.");

			auto created = std::make_unique<MiniaudioVoice>();
			created->payload = clip->payload;
			created->identity = command.identity;
			ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0U, 0U);
			if(ma_decoder_init_memory(created->payload->data(), created->payload->size(), &decoderConfig, &created->decoder) != MA_SUCCESS)
				return {AudioResultCode::ProviderFailure, "miniaudio rejected an encoded clip; the frame packet was not committed."};
			created->decoderInitialized = true;
			if(ma_sound_init_from_data_source(&impl->engine, &created->decoder, 0U, nullptr, &created->sound) != MA_SUCCESS)
				return {AudioResultCode::ProviderFailure, "miniaudio could not stage a voice; the frame packet was not committed."};
			created->soundInitialized = true;
			stagedVoices[index] = std::move(created);
		}

		for(const auto& registration : packet.registrations)
			impl->clips[ClipKey(registration.assetId, registration.generation)] = registration;
		for(const auto& release : packet.releases) impl->clips.erase(ClipKey(release.assetId, release.generation));
		ma_engine_set_volume(&impl->engine, packet.masterMuted ? 0.0F : NormalizeAudioUnit(packet.masterGain));

		for(std::size_t commandIndex = 0U; commandIndex < packet.voices.size(); ++commandIndex) {
			const auto& command = packet.voices[commandIndex];
			if(command.destroy) { impl->voices.erase(command.voice); continue; }
			auto voice = impl->voices.find(command.voice);
			bool newlyCreated = false;
			if(voice == impl->voices.end()) {
				auto created = std::move(stagedVoices[commandIndex]);
				if(!created)
					return {AudioResultCode::ProviderFailure, "A preflighted voice was unavailable during atomic packet commit."};
				voice = impl->voices.insert_or_assign(command.voice, std::move(created)).first;
				newlyCreated = true;
			}

			MiniaudioVoice& active = *voice->second;
			ma_sound_set_volume(&active.sound, NormalizeAudioUnit(command.gain));
			ma_sound_set_pitch(&active.sound, NormalizeAudioPitch(command.pitch));
			ma_sound_set_looping(&active.sound, command.loop ? MA_TRUE : MA_FALSE);
			ma_sound_set_pan(&active.sound, std::clamp(command.spatial.pan, -1.0F, 1.0F));
			// TerraEngine has already applied its authored min/max-distance curve and
			// spatial blend to the final gain and pan. Disable miniaudio's independent
			// 3D attenuation so the concrete provider cannot attenuate the voice twice.
			ma_sound_set_spatialization_enabled(&active.sound, MA_FALSE);
			ma_sound_set_position(&active.sound, command.spatial.position.x, command.spatial.position.y, command.spatial.position.z);
			const bool shouldPause = command.paused || packet.gamePaused;
			if(!shouldPause && (newlyCreated || active.wasPaused))
				(void)ma_sound_seek_to_pcm_frame(&active.sound, command.seekFrame);
			if(shouldPause) (void)ma_sound_stop(&active.sound);
			else if(!ma_sound_is_playing(&active.sound) && !ma_sound_at_end(&active.sound)) (void)ma_sound_start(&active.sound);
			active.wasPaused = shouldPause;
			if(ma_sound_at_end(&active.sound) && !command.loop)
				impl->PushProviderObservation({AudioBackendObservationKind::VoiceCompleted, command.voice, command.identity, "Voice playback completed."});
		}

		impl->lastSequence = packet.sequence;
		if(impl->recoveryPublicationPending) {
			impl->PushProviderObservation({AudioBackendObservationKind::DeviceRecovered, 0U, {}, "The playback device was recovered and the frame packet reconciled active voices."});
			impl->recoveryPublicationPending = false;
		}
		return AudioResult::Accepted();
	}

	AudioResult MiniaudioBackend::PollObservations(std::span<AudioBackendObservation> destination, std::size_t& requiredCount) {
		const std::uint64_t callbackRead = impl->callbackRead.load(std::memory_order_relaxed);
		const std::uint64_t callbackWrite = impl->callbackWrite.load(std::memory_order_acquire);
		const std::uint64_t callbackCount = callbackWrite - callbackRead;
		impl->pendingCallbackDrops += impl->callbackDrops.exchange(0U, std::memory_order_acq_rel);
		requiredCount = static_cast<std::size_t>(callbackCount) + impl->providerCount + (impl->pendingCallbackDrops != 0U || impl->providerDrops != 0U ? 1U : 0U);
		std::size_t copied = 0U;
		const auto copy = [&](AudioBackendObservation observation) {
			if(copied < destination.size()) destination[copied++] = std::move(observation);
		};
		for(std::uint64_t index = 0U; index < callbackCount && copied < destination.size(); ++index) {
			const auto raw = static_cast<CallbackObservation>(impl->callbackObservations[(callbackRead + index) % kMaximumObservations].load(std::memory_order_relaxed));
			if(raw == CallbackObservation::DeviceLost)
				copy({AudioBackendObservationKind::DeviceLost, 0U, {}, "The playback device stopped, was rerouted, or was interrupted."});
		}
		const std::uint64_t callbackConsumed = std::min<std::uint64_t>(callbackCount, copied);
		impl->callbackRead.store(callbackRead + callbackConsumed, std::memory_order_release);
		while(impl->providerCount != 0U && copied < destination.size()) {
			copy(std::move(impl->providerObservations[impl->providerRead]));
			impl->providerRead = (impl->providerRead + 1U) % kMaximumObservations;
			--impl->providerCount;
		}
		if((impl->pendingCallbackDrops != 0U || impl->providerDrops != 0U) && copied < destination.size()) {
			copy({AudioBackendObservationKind::Overflow, 0U, {}, "The provider observation channel overflowed."});
			impl->pendingCallbackDrops = 0U;
			impl->providerDrops = 0U;
		}
		return copied < requiredCount
			? AudioResult{AudioResultCode::CapacityExceeded, "The caller observation buffer is too small."}
			: AudioResult::Accepted();
	}

	AudioResult MiniaudioBackend::Shutdown() {
		if(!impl->initialized) return AudioResult::Accepted();
		// Disable callback publication before stopping the device, then uninitialize
		// in dependency order: voices -> engine graph -> externally owned device.
		impl->acceptsCallbacks.store(false, std::memory_order_release);
		impl->voices.clear();
		impl->clips.clear();
		if(impl->deviceInitialized) (void)ma_device_stop(&impl->device);
		if(impl->engineInitialized) ma_engine_uninit(&impl->engine);
		if(impl->deviceInitialized) ma_device_uninit(&impl->device);
		impl->engineInitialized = false;
		impl->deviceInitialized = false;
		impl->callbackRead.store(0U, std::memory_order_relaxed);
		impl->callbackWrite.store(0U, std::memory_order_relaxed);
		impl->callbackDrops.store(0U, std::memory_order_relaxed);
		impl->callbackProducer.clear(std::memory_order_relaxed);
		impl->deviceLost.store(false, std::memory_order_relaxed);
		impl->recoveryPublicationPending = false;
		impl->pendingCallbackDrops = 0U;
		impl->providerRead = 0U;
		impl->providerWrite = 0U;
		impl->providerCount = 0U;
		impl->providerDrops = 0U;
		impl->initialized = false;
		impl->lastSequence = 0U;
		return AudioResult::Accepted();
	}

	AudioProviderDescriptor MiniaudioProviderDescriptor() {
		return {"terra.miniaudio", "miniaudio", kAudioProviderContractVersion, "0.11.25",
			"9634bedb5b5a2ca38c1ee7108a9358a4e233f14d", kRequiredAudioProviderCapabilities, 128U};
	}

	void RegisterMiniaudioBackend() {
		(void)GlobalAudioBackendRegistry().RegisterProvider(
			MiniaudioProviderDescriptor(), [] { return std::make_unique<MiniaudioBackend>(); });
	}
}
