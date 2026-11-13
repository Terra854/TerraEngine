// Production-provider graph regression; no operating-system audio device opens.
#include "TestHarness.h"
#include "Audio/Backend/Detail/MiniaudioOfflineProbe.h"
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
namespace {
	using namespace Audio;
	using Probe = Audio::Backend::detail::MiniaudioOfflineProbe;
	struct Rendered {
		std::array<float, 512U * kAudioRenderChannelCount> samples{};
		std::uint32_t inputChannels = 0;
	};
	Rendered RenderStereo(TestContext& context, bool spatial) {
		AudioBackendCreateInfo info; info.sampleRate = 8000U;
		AudioResult initialized;
		auto backend = Probe::Create(info, initialized);
		context.Expect(initialized.Succeeded() && backend != nullptr, "The actual provider must create its no-device engine graph.");
		// A long constant PCM16 stereo wave gives an independent exact oracle.
		// Keep enough frames to observe steady state after the provider's ordinary
		// pitch/converter startup; the test must not confuse that with channel gain.
		std::vector<std::uint8_t> wave;
		const auto text = [&](const char* value) { for(unsigned i = 0; i < 4; ++i) wave.push_back(static_cast<std::uint8_t>(value[i])); };
		const auto u16 = [&](std::uint16_t value) { wave.push_back(static_cast<std::uint8_t>(value)); wave.push_back(static_cast<std::uint8_t>(value >> 8U)); };
		const auto u32 = [&](std::uint32_t value) { for(unsigned i = 0; i < 4; ++i) wave.push_back(static_cast<std::uint8_t>(value >> (i * 8U))); };
		text("RIFF"); u32(36U + 1024U * 4U); text("WAVE"); text("fmt "); u32(16U); u16(1U); u16(2U); u32(8000U); u32(32000U); u16(4U); u16(16U); text("data"); u32(1024U * 4U);
		for(unsigned frame = 0; frame < 1024U; ++frame) { u16(16384U); u16(8192U); }
		auto payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(wave));
		AudioClipRegistration clip;
		clip.assetId = TerraEngine::UUID{91U,3U}; clip.generation = 1U; clip.sampleRate = 8000U; clip.channels = 2U; clip.frameCount = 1024U; clip.payload = payload;
		AudioVoiceCommand voice;
		voice.voice = 7U; voice.identity = {TerraEngine::UUID{92U,3U}, 1U, 1U}; voice.clipAssetId = clip.assetId; voice.clipGeneration = 1U; voice.create = true;
		voice.spatial.spatialBlend = spatial ? 1.0F : 0.0F;
		voice.spatial.speakerGains[0U] = 1.0F;
		AudioBackendFramePacket packet; packet.sequence = 1U; packet.registrations.push_back(clip); packet.voices.push_back(voice);
		context.Expect(backend->SubmitFrame(packet).Succeeded(), "The actual packet/decoder/sound/custom-node path must admit the stereo voice.");
		Rendered result; result.inputChannels = Probe::InputChannels(*backend, voice.voice);
		context.Expect(Probe::ReadFrames(*backend, result.samples, 512U).Succeeded(), "The actual graph must produce the requested no-device PCM frames.");
		context.Expect(backend->Shutdown().Succeeded(), "Offline provider teardown must settle its owned graph.");
		return result;
	}
}
void RegisterSpatialDecoderChannelTests(TestRegistry& registry) {
	registry.Add("Audio.SpatialDecoder.PointSourceMatchesIndependentStereoAmplitude", [](TestContext& context) {
		const auto result = RenderStereo(context, true);
		// Fixture values are left=1/2 and right=1/4. A point source
		// averages the authored stereo pair rather than eight upmixed channels.
		constexpr float expected = 0.375F;
		for(unsigned frame = 256U; frame < 512U; ++frame) {
			context.Expect(std::abs(result.samples[frame * kAudioRenderChannelCount] - expected) < 1e-5F,
				"Point-source gain must preserve the independent (left+right)/2 reference.");
		}
		context.ExpectEqual(2U, result.inputChannels, "The actual custom node must receive the stereo decoder channel contract.");
	});
	registry.Add("Audio.SpatialDecoder.NonSpatialStereoControl", [](TestContext& context) {
		const auto result = RenderStereo(context, false);
		for(unsigned frame = 256U; frame < 512U; ++frame) {
			context.Expect(std::abs(result.samples[frame * kAudioRenderChannelCount] - 0.5F) < 1e-5F
				&& std::abs(result.samples[frame * kAudioRenderChannelCount + 1U] - 0.25F) < 1e-5F,
				"The dry stereo control must retain each independently authored steady-state channel amplitude.");
		}
	});
}
}
