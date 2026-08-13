// AudioOfflineRenderTests.cpp - validates real decode and mixing without opening an output device.

#include "../TestHarness.h"

#include <miniaudio.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		constexpr ma_uint32 kFixtureChannels = 1U;
		constexpr ma_uint32 kFixtureSampleRate = 8000U;
		constexpr std::size_t kFixtureFrameCount = 8U;

		// The session owns miniaudio objects in dependency order. Destruction runs in
		// reverse so the sound never observes a released decoder or engine.
		struct OfflineRenderSession final {
			ma_engine engine{};
			ma_decoder decoder{};
			ma_sound sound{};
			bool engineInitialized = false;
			bool decoderInitialized = false;
			bool soundInitialized = false;

			~OfflineRenderSession() {
				if(soundInitialized) {
					ma_sound_uninit(&sound);
				}
				if(decoderInitialized) {
					ma_decoder_uninit(&decoder);
				}
				if(engineInitialized) {
					ma_engine_uninit(&engine);
				}
			}
		};

		std::vector<std::uint8_t> ReadFixtureBytes(
			TestContext& context,
			const std::filesystem::path& relativePath) {
			const std::filesystem::path fixturePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream input(fixturePath, std::ios::binary);
			context.Expect(input.good(), "The committed audio fixture must be readable.");
			return std::vector<std::uint8_t>(
				std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
		}

		std::array<float, kFixtureFrameCount> RenderKnownFixture(TestContext& context) {
			const std::vector<std::uint8_t> encodedBytes = ReadFixtureBytes(
				context,
				std::filesystem::path("Tests") / "Fixtures" / "Audio" / "Generated" / "mono_pcm16.wav");
			context.Expect(!encodedBytes.empty(), "The offline-render fixture must contain encoded WAV bytes.");

			OfflineRenderSession session;
			ma_engine_config engineConfig = ma_engine_config_init();
			engineConfig.noDevice = MA_TRUE;
			engineConfig.channels = kFixtureChannels;
			engineConfig.sampleRate = kFixtureSampleRate;
			context.Expect(ma_engine_init(&engineConfig, &session.engine) == MA_SUCCESS,
				"A no-device miniaudio engine must initialize for offline mixing.");
			session.engineInitialized = true;
			context.Expect(ma_engine_get_device(&session.engine) == nullptr,
				"Offline validation must not create or open an operating-system audio device.");

			const ma_decoder_config decoderConfig = ma_decoder_config_init(
				ma_format_f32,
				kFixtureChannels,
				kFixtureSampleRate);
			context.Expect(ma_decoder_init_memory(
				encodedBytes.data(),
				encodedBytes.size(),
				&decoderConfig,
				&session.decoder) == MA_SUCCESS,
				"The production miniaudio decoder must accept the committed WAV fixture.");
			session.decoderInitialized = true;

			constexpr ma_uint32 soundFlags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
			context.Expect(ma_sound_init_from_data_source(
				&session.engine,
				reinterpret_cast<ma_data_source*>(&session.decoder),
				soundFlags,
				nullptr,
				&session.sound) == MA_SUCCESS,
				"The decoded fixture must attach to the no-device engine graph.");
			session.soundInitialized = true;
			context.Expect(ma_sound_start(&session.sound) == MA_SUCCESS,
				"The offline sound must enter the playing state before mixing.");

			std::array<float, kFixtureFrameCount> renderedFrames{};
			ma_uint64 framesRead = 0U;
			context.Expect(ma_engine_read_pcm_frames(
				&session.engine,
				renderedFrames.data(),
				renderedFrames.size(),
				&framesRead) == MA_SUCCESS,
				"Manual no-device mixing must produce PCM frames in memory.");
			context.ExpectEqual(static_cast<ma_uint64>(renderedFrames.size()), framesRead,
				"The offline mixer must fill the requested frame block.");
			return renderedFrames;
		}
	}

	void RegisterAudioOfflineRenderTests(TestRegistry& registry) {
		registry.Add("Audio.OfflineRender.DecodesKnownWaveformWithoutDevice", [](TestContext& context) {
			const std::array<float, kFixtureFrameCount> renderedFrames = RenderKnownFixture(context);
			constexpr std::array<std::int16_t, kFixtureFrameCount> expectedPcm16{
				0, 1000, -1000, 0, 500, -500, 0, 0
			};
			constexpr float kPcm16Scale = 1.0F / 32768.0F;
			constexpr float kTolerance = 0.000001F;
			float energy = 0.0F;

			for(std::size_t frameIndex = 0; frameIndex < renderedFrames.size(); ++frameIndex) {
				const float expectedSample = static_cast<float>(expectedPcm16[frameIndex]) * kPcm16Scale;
				const float actualSample = renderedFrames[frameIndex];
				context.Expect(std::isfinite(actualSample), "Offline mixing must never emit NaN or infinity.");
				context.Expect(std::abs(actualSample - expectedSample) <= kTolerance,
					"Decoded PCM must match the known committed waveform within float conversion tolerance.");
				energy += actualSample * actualSample;
			}

			context.Expect(energy > 0.001F,
				"The rendered block must contain measurable non-silent signal energy.");
		});

		registry.Add("Audio.OfflineRender.RepeatedMixIsDeterministic", [](TestContext& context) {
			const std::array<float, kFixtureFrameCount> firstRender = RenderKnownFixture(context);
			const std::array<float, kFixtureFrameCount> secondRender = RenderKnownFixture(context);
			context.Expect(firstRender == secondRender,
				"Two independent no-device sessions must render the same fixture identically.");
		});

		registry.Add("Audio.OfflineRender.DecodesCommittedOggVorbis", [](TestContext& context) {
			const std::vector<std::uint8_t> encodedBytes = ReadFixtureBytes(
				context,
				std::filesystem::path("Tests") / "Fixtures" / "Audio" / "ThirdParty"
					/ "Kenney" / "Music" / "jingles_NES00.ogg");
			context.Expect(!encodedBytes.empty(), "The committed Ogg fixture must contain encoded bytes.");

			OfflineRenderSession session;
			ma_engine_config engineConfig = ma_engine_config_init();
			engineConfig.noDevice = MA_TRUE;
			engineConfig.channels = 2U;
			engineConfig.sampleRate = 44100U;
			context.Expect(ma_engine_init(&engineConfig, &session.engine) == MA_SUCCESS,
				"The Ogg regression must initialize a no-device engine.");
			session.engineInitialized = true;
			context.Expect(ma_engine_get_device(&session.engine) == nullptr,
				"The Ogg regression must remain safe for muted and headless runners.");

			const ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 2U, 44100U);
			context.Expect(ma_decoder_init_memory(
				encodedBytes.data(),
				encodedBytes.size(),
				&decoderConfig,
				&session.decoder) == MA_SUCCESS,
				"The production miniaudio build must register its bundled Ogg Vorbis decoder.");
			session.decoderInitialized = true;

			constexpr ma_uint32 soundFlags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
			context.Expect(ma_sound_init_from_data_source(
				&session.engine,
				reinterpret_cast<ma_data_source*>(&session.decoder),
				soundFlags,
				nullptr,
				&session.sound) == MA_SUCCESS,
				"The decoded Ogg fixture must attach to the no-device engine graph.");
			session.soundInitialized = true;
			context.Expect(ma_sound_start(&session.sound) == MA_SUCCESS,
				"The Ogg regression sound must start before manual mixing.");

			constexpr ma_uint64 kOggFramesToRender = 4096U;
			std::vector<float> renderedSamples(kOggFramesToRender * 2U, 0.0F);
			ma_uint64 framesRead = 0U;
			context.Expect(ma_engine_read_pcm_frames(
				&session.engine,
				renderedSamples.data(),
				kOggFramesToRender,
				&framesRead) == MA_SUCCESS,
				"The bundled Ogg decoder must feed the manual mixer.");
			context.ExpectEqual(kOggFramesToRender, framesRead,
				"The Ogg offline mixer must fill the requested frame block.");

			float energy = 0.0F;
			for(const float sample : renderedSamples) {
				context.Expect(std::isfinite(sample), "Ogg decoding must not emit NaN or infinity.");
				energy += sample * sample;
			}
			context.Expect(energy > 0.001F,
				"The committed Ogg fixture must decode into measurable non-silent signal energy.");
		});
	}
}
