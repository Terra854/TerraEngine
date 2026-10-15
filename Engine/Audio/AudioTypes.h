// AudioTypes.h - provider-neutral audio identities, packets, and validation.
//
// These values deliberately contain no decoder or device SDK types. The engine
// owns logical clocks and ordering; providers only consume complete packets.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Audio {
	using AudioGeneration = std::uint64_t;
	using AudioFrame = std::uint64_t;
	using AudioPacketSequence = std::uint64_t;
	using AudioVoiceToken = std::uint64_t;
	// Version 2 adds listener-selected render modes, frontness, and explicit
	// speaker gains to the provider packet. Version-1 providers must reject rather
	// than interpreting the expanded spatial command with the old ABI.
	inline constexpr std::uint32_t kAudioProviderContractVersion = 2U;
	inline constexpr std::uint32_t kAudioRenderChannelCount = 8U;

	// This copied observation identifies one component attachment, independently
	// of transient ECS storage and the individual playback instances it owns.
	struct AudioSourceAttachmentKey {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		AudioGeneration sceneGeneration = 0U;
		AudioGeneration objectGeneration = 0U;
		AudioGeneration componentGeneration = 0U;
		bool IsValid() const noexcept {
			return objectId != TerraEngine::EMPTY_UUID && sceneGeneration != 0U
				&& objectGeneration != 0U && componentGeneration != 0U;
		}
		auto operator<=>(const AudioSourceAttachmentKey&) const = default;
	};
	struct AudioSourceBinding {
		std::uint32_t objectEntity = 0U;
		AudioSourceAttachmentKey attachment;
	};

	// One effective listener defines the player's output experience. These values
	// are stable durable authoring data; an omitted field remains Stereo.
	enum class AudioOutputMode : std::uint8_t {
		Mono = 0U,
		Stereo = 1U,
		Hrtf = 2U,
		Surround5Point1 = 3U,
		Surround7Point1 = 4U,
	};

	struct AudioVector3 {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
		auto operator<=>(const AudioVector3&) const = default;
	};

	struct AudioSourceIdentity {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		AudioGeneration componentGeneration = 0U;
		AudioGeneration playbackGeneration = 0U;
		bool IsValid() const noexcept { return objectId != TerraEngine::EMPTY_UUID && componentGeneration != 0U; }
		auto operator<=>(const AudioSourceIdentity&) const = default;
	};

	enum class AudioResultCode : std::uint8_t { Success, Accepted, Rejected, InvalidState, StalePacket, CapacityExceeded, Unsupported, ProviderFailure, DeviceUnavailable };
	struct AudioResult {
		AudioResultCode code = AudioResultCode::Success;
		std::string message;
		bool Succeeded() const noexcept { return code == AudioResultCode::Success || code == AudioResultCode::Accepted; }
		static AudioResult Accepted(std::string message = {}) { return {AudioResultCode::Accepted, std::move(message)}; }
		static AudioResult Rejected(std::string message) { return {AudioResultCode::Rejected, std::move(message)}; }
	};

	enum class AudioClipEncoding : std::uint8_t { Wav, OggVorbis };
	enum class AudioPlaybackState : std::uint8_t { PendingAsset, Playing, Paused, Stopped, Completed, Failed };
	enum class AudioAudibility : std::uint8_t { Audible, MasterMuted, GroupMuted, SourceMuted, OutOfRange, VoiceLimited, NoListener, DeviceUnavailable, PendingAsset, Failed };
	enum class AudioBackendObservationKind : std::uint8_t { DeviceLost, DeviceRecovered, VoiceCompleted, ClipRejected, Overflow, ProviderFailure };

	enum class AudioProviderCapability : std::uint32_t {
		None = 0U, Wav = 1U << 0U, OggVorbis = 1U << 1U, Voices = 1U << 2U,
		Seek = 1U << 3U, Loop = 1U << 4U, Gain = 1U << 5U, Pitch = 1U << 6U,
		Spatial = 1U << 7U, DeviceEvents = 1U << 8U, Hrtf = 1U << 9U,
		Surround = 1U << 10U
	};
	constexpr AudioProviderCapability operator|(AudioProviderCapability left, AudioProviderCapability right) noexcept {
		return static_cast<AudioProviderCapability>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}
	constexpr bool HasAudioCapability(AudioProviderCapability values, AudioProviderCapability required) noexcept {
		return (static_cast<std::uint32_t>(values) & static_cast<std::uint32_t>(required)) == static_cast<std::uint32_t>(required);
	}
	inline constexpr AudioProviderCapability kRequiredAudioProviderCapabilities = AudioProviderCapability::Wav | AudioProviderCapability::OggVorbis | AudioProviderCapability::Voices | AudioProviderCapability::Seek | AudioProviderCapability::Loop | AudioProviderCapability::Gain | AudioProviderCapability::Pitch | AudioProviderCapability::Spatial | AudioProviderCapability::DeviceEvents;

	struct AudioProviderDescriptor {
		std::string providerId;
		std::string displayName;
		std::uint32_t contractVersion = kAudioProviderContractVersion;
		std::string providerVersion;
		std::string providerRevision;
		AudioProviderCapability capabilities = AudioProviderCapability::None;
		std::uint32_t maximumVoices = 0U;
		bool IsComplete() const noexcept { return !providerId.empty() && !displayName.empty() && maximumVoices != 0U; }
		bool IsCompatible() const noexcept { return IsComplete() && contractVersion == kAudioProviderContractVersion && HasAudioCapability(capabilities, kRequiredAudioProviderCapabilities); }
	};

	struct AudioBackendCreateInfo { std::uint32_t sampleRate = 48000U; std::uint32_t channels = kAudioRenderChannelCount; std::uint32_t maximumVoices = 64U; bool requestDeviceRecovery = true; };
	struct AudioClipRegistration {
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AudioGeneration generation = 0U;
		AudioClipEncoding encoding = AudioClipEncoding::Wav;
		std::uint32_t sampleRate = 0U, channels = 0U;
		AudioFrame frameCount = 0U;
		std::uint64_t checksum = 0U;
		std::shared_ptr<const std::vector<std::uint8_t>> payload;
		bool IsValid() const noexcept { return assetId != TerraEngine::EMPTY_UUID && generation != 0U && sampleRate != 0U && channels != 0U && payload != nullptr; }
	};
	struct AudioClipRelease { TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID; AudioGeneration generation = 0U; };
	struct AudioSpatialParameters {
		AudioVector3 position;
		AudioVector3 forward{0.0F, 0.0F, -1.0F};
		std::array<float, kAudioRenderChannelCount> speakerGains{};
		AudioOutputMode outputMode = AudioOutputMode::Stereo;
		float pan = 0.0F;
		float frontness = 1.0F;
		float spatialBlend = 0.0F;
	};
	struct AudioVoiceCommand {
		AudioVoiceToken voice = 0U;
		AudioSourceIdentity identity;
		TerraEngine::UUID clipAssetId = TerraEngine::EMPTY_UUID;
		AudioGeneration clipGeneration = 0U;
		AudioFrame seekFrame = 0U;
		float gain = 1.0F, pitch = 1.0F;
		bool create = false, destroy = false, paused = false, loop = false;
		AudioSpatialParameters spatial;
	};
	struct AudioBackendFramePacket {
		AudioPacketSequence sequence = 0U;
		AudioFrame timelineFrame = 0U;
		std::vector<AudioClipRegistration> registrations;
		std::vector<AudioClipRelease> releases;
		std::vector<AudioVoiceCommand> voices;
		float masterGain = 1.0F;
		bool masterMuted = false, gamePaused = false, requestRecovery = false;
	};
	struct AudioBackendObservation { AudioBackendObservationKind kind = AudioBackendObservationKind::ProviderFailure; AudioVoiceToken voice = 0U; AudioSourceIdentity identity; std::string message; };

	inline float NormalizeAudioUnit(float value) noexcept { return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F; }
	inline float NormalizeAudioPitch(float value) noexcept { return std::isfinite(value) ? std::clamp(value, 0.01F, 4.0F) : 1.0F; }
}
