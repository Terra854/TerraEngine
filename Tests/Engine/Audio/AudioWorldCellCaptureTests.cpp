// Production component capture must preserve exact cells until listener-relative preparation.
#include "../TestHarness.h"
#include "FakeAudioBackend.h"
#include "Audio/AudioSystem.h"
#include "Audio/Backend/AudioBackendRegistry.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Scheduling/JobGraph.h"
#include "File Formats/TerraEngineAudioFile.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

namespace TerraEngine::Tests {
	namespace {
		namespace NativeAudio = TerraEngine::Audio;
		constexpr const char* kCaptureProvider = "test.worldcellcapture";
		Audio::FakeAudioBackend* recordingProvider = nullptr;
		bool providerRegistered = false;

		// A real WAV is wrapped in the native envelope and loaded through ResourceManager.
		// Eight silent mono frames suffice because zero-duration ticks test geometry only.
		std::vector<std::uint8_t> MakeWaveBytes() {
			std::vector<std::uint8_t> bytes;
			const auto text = [&](const char* value) { for(int i = 0; i < 4; ++i) bytes.push_back(value[i]); };
			const auto word = [&](std::uint32_t value, unsigned count) {
				for(unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
			};
			text("RIFF"); word(52U, 4U); text("WAVE"); text("fmt "); word(16U, 4U);
			word(1U, 2U); word(1U, 2U); word(8000U, 4U); word(16000U, 4U); word(2U, 2U); word(16U, 2U);
			text("data"); word(16U, 4U); bytes.resize(bytes.size() + 16U);
			return bytes;
		}

		// This fixture owns the device-independent provider, its live components and its
		// disposable asset. Cleanup also runs after a failed assertion, avoiding test-order leaks.
		struct CaptureFixture final {
			std::filesystem::path directory;
			TerraEngine::UUID clipId = TerraEngine::EMPTY_UUID;
			NativeAudio::AudioMixSnapshot previousMix;
			bool ownsWorkerPool = false;
			Object source;
			Object listener;
			CaptureFixture(TestContext& context) {
				auto& system = NativeAudio::AudioSystem::Instance();
				system.Shutdown();
				previousMix = system.LatestMixSnapshot();
				std::string mixDiagnostic;
				context.Expect(system.ReplaceMixSnapshot({}, mixDiagnostic), "Each fixture must start with an unmuted unit-gain mix.");
				ObjectManager::instance().clear();
				context.Expect(providerRegistered, "The recording provider must register before the global registry freezes.");
				directory = std::filesystem::temp_directory_path() / ("terra-audio-capture-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
				std::filesystem::create_directories(directory);
				const auto payload = MakeWaveBytes();
				NativeAudio::AudioFileMetadata metadata;
				metadata.assetId = TerraEngine::GenerateUUID(); metadata.encoding = NativeAudio::AudioEncoding::Wave;
				metadata.channelCount = 1U; metadata.sampleRate = 8000U; metadata.frameCount = 8U;
				metadata.payloadChecksum = NativeAudio::ComputeAudioChecksum(payload);
				std::vector<std::uint8_t> envelope;
				context.Expect(NativeAudio::SerializeAudioFile(metadata, payload, envelope) == NativeAudio::AudioFileStatus::Ok,
					"The fresh WAV envelope must serialize.");
				const auto path = directory / "capture.teaudio";
				{ std::ofstream output(path, std::ios::binary); output.write(reinterpret_cast<const char*>(envelope.data()), static_cast<std::streamsize>(envelope.size()));
					context.Expect(output.good(), "The disposable native audio asset must be written completely."); }
				clipId = ResourceManager::Instance().LoadAudioClip(path);
				context.Expect(clipId == metadata.assetId, "Production loading must publish the fresh clip generation.");
				auto& objects = ObjectManager::instance();
				source = objects.createObject("Captured sound"); listener = objects.createObject("Captured listener");
				auto* sourceComponent = objects.AddComponent<AudioSourceComponent>(source);
				context.Expect(sourceComponent != nullptr && objects.AddComponent<AudioListenerComponent>(listener) != nullptr,
					"Real source and listener components must be available for production capture.");
				sourceComponent->clipAssetId = clipId; sourceComponent->playOnStart = true; sourceComponent->loop = true;
				sourceComponent->maxDistance = 50.0F;
				system.SetRuntimePlaybackEnabled(true); system.SetConfiguredProvider(kCaptureProvider); system.Initialize();
				context.Expect(recordingProvider != nullptr, "AudioSystem must construct the device-independent recording provider.");
			}
			~CaptureFixture() {
				// A filtered harness has no Engine owner to join workers we started.
				// Settle them before releasing audio/provider inputs, including failures.
				if(ownsWorkerPool) ThreadPool::instance().shutdown();
				NativeAudio::AudioSystem::Instance().Shutdown(); recordingProvider = nullptr;
				NativeAudio::AudioSystem::Instance().SetConfiguredProvider("terra.miniaudio");
				std::string mixDiagnostic;
				(void)NativeAudio::AudioSystem::Instance().ReplaceMixSnapshot(std::move(previousMix), mixDiagnostic);
				ObjectManager::instance().clear();
				if(clipId != TerraEngine::EMPTY_UUID) ResourceManager::Instance().DeleteResource(clipId, AssetType::AudioClip);
				std::error_code error; if(!directory.empty()) std::filesystem::remove_all(directory, error);
			}
			void EnsureWorkerPool() {
				if(ThreadPool::instance().getThreadCount() == 0U) {
					ownsWorkerPool = true;
					ThreadPool::instance().initialize(2U);
				}
			}
			void Place(TestContext& context, const Object& object, Scene::WorldPosition position) {
				Scene::WorldPose pose; pose.position = position;
				context.Expect(ObjectManager::instance().SetCanonicalWorldPose(object, pose) == Scene::WorldCoordinateError::None,
					"The real component fixture must retain its canonical world position.");
			}
			NativeAudio::AudioPreparedVoice Tick(TestContext& context) {
				auto& system = NativeAudio::AudioSystem::Instance();
				const auto result = Scheduling::ExecuteFrameWorkloads(system.DeclareFrameWorkloads(0.0F), "audio-cell-capture");
				context.Expect(result.valid && result.completed, "The production capture, worker prepare and protected commit graph must complete.");
				const auto publication = system.LatestPlaybackSnapshot();
				context.Expect(publication && publication->voices.size() == 1U, "Production publication must contain the captured source.");
				context.Expect(!recordingProvider->Packets().empty(), "Protected commit must submit a real provider packet.");
				for(const auto& command : recordingProvider->Packets().back().voices)
					context.Expect(std::isfinite(command.spatial.position.x) && std::isfinite(command.spatial.position.y)
						&& std::isfinite(command.spatial.position.z), "Every emitted provider coordinate must be finite.");
				return publication->voices.front();
			}
		};

		void ExpectTwoMetres(TestContext& context, const NativeAudio::AudioPreparedVoice& voice, float direction) {
			context.Expect(voice.audibility == NativeAudio::AudioAudibility::Audible, "A two-metre source across the cell edge must remain audible.");
			context.Expect(std::abs(voice.gain - 0.9216F) <= 0.0001F, "The fifty-metre quadratic attenuation must yield 0.9216 at two metres.");
			context.Expect(std::abs(voice.spatial.pan - direction) <= 0.0001F && std::abs(voice.spatial.frontness) <= 0.0001F,
				"Boundary capture must preserve listener-relative direction.");
			context.Expect(std::abs(voice.spatial.position.x - direction * 2.0F) <= 0.0001F,
				"Provider positions must use the selected listener as their shared zero origin.");
		}
	}

	void RegisterAudioWorldCellCaptureTests(TestRegistry& registry) {
		// Registration happens while building the test registry, before any test can
		// initialize AudioSystem and freeze startup providers. No live override is used.
		auto& providers = NativeAudio::Backend::GlobalAudioBackendRegistry();
		if(!providers.Find(kCaptureProvider)) {
			providerRegistered = providers.RegisterProvider({kCaptureProvider, "World cell capture recorder", NativeAudio::kAudioProviderContractVersion,
				"1", "fixture", NativeAudio::kRequiredAudioProviderCapabilities, 64U}, [] {
				auto provider = std::make_unique<Audio::FakeAudioBackend>(); recordingProvider = provider.get(); return provider;
			}).result.Succeeded();
		} else providerRegistered = true;

		registry.Add("Audio.WorldCellCapture.PositiveBoundaryMatchesNearbyReference", [](TestContext& context) {
			CaptureFixture fixture(context);
			fixture.Place(context, fixture.listener, {{0, 0, 0}, {0.0, 0.0, 0.0}});
			fixture.Place(context, fixture.source, {{0, 0, 0}, {2.0, 0.0, 0.0}});
			const auto nearby = fixture.Tick(context); ExpectTwoMetres(context, nearby, 1.0F);
			fixture.Place(context, fixture.listener, {{0, 0, 0}, {32767.0, 0.0, 0.0}});
			fixture.Place(context, fixture.source, {{1, 0, 0}, {-32767.0, 0.0, 0.0}});
			const auto boundary = fixture.Tick(context); ExpectTwoMetres(context, boundary, 1.0F);
			context.Expect(std::abs(boundary.gain - nearby.gain) <= 0.0001F, "Cross-cell and nearby arrangements must mix identically through production capture.");
			context.Expect(!recordingProvider->Packets().back().voices.empty(), "The audible boundary source must reach the provider.");
		});
		registry.Add("Audio.WorldCellCapture.NegativeBoundaryAndExtremeCommonTranslations", [](TestContext& context) {
			CaptureFixture fixture(context);
			for(const auto cell : {std::int64_t{0}, std::numeric_limits<std::int64_t>::max() - 1, std::numeric_limits<std::int64_t>::min() + 1}) {
				fixture.Place(context, fixture.listener, {{cell, 0, 0}, {32767.0, 0.0, 0.0}});
				fixture.Place(context, fixture.source, {{cell + 1, 0, 0}, {-32767.0, 0.0, 0.0}});
				ExpectTwoMetres(context, fixture.Tick(context), 1.0F);
				fixture.Place(context, fixture.listener, {{cell, 0, 0}, {-32767.0, 0.0, 0.0}});
				fixture.Place(context, fixture.source, {{cell - 1, 0, 0}, {32767.0, 0.0, 0.0}});
				ExpectTwoMetres(context, fixture.Tick(context), -1.0F);
			}
		});
		registry.Add("Audio.WorldCellCapture.NoListenerNonSpatialAndTrueRangeExclusion", [](TestContext& context) {
			CaptureFixture fixture(context);
			fixture.Place(context, fixture.listener, {{std::numeric_limits<std::int64_t>::min(), 0, 0}, {0.0, 0.0, 0.0}});
			fixture.Place(context, fixture.source, {{std::numeric_limits<std::int64_t>::max(), 0, 0}, {0.0, 0.0, 0.0}});
			context.Expect(fixture.Tick(context).audibility == NativeAudio::AudioAudibility::OutOfRange,
				"Opposite extreme cells must reject range without overflowing or becoming coincident.");
			ObjectManager::instance().GetComponent<AudioListenerComponent>(fixture.listener)->eligible = false;
			context.Expect(fixture.Tick(context).audibility == NativeAudio::AudioAudibility::NoListener, "Spatial playback without an eligible listener remains safely silent.");
			ObjectManager::instance().GetComponent<AudioSourceComponent>(fixture.source)->spatialBlend = 0.0F;
			const auto nonSpatial = fixture.Tick(context);
			context.Expect(nonSpatial.audibility == NativeAudio::AudioAudibility::Audible && std::abs(nonSpatial.gain - 1.0F) <= 0.0001F,
				"Non-spatial playback bypasses coordinate localization and listener requirements.");
		});
		registry.Add("Audio.WorldCellCapture.ListenerSelectionReplacementAndProtectedPublication", [](TestContext& context) {
			CaptureFixture fixture(context);
			auto& objects = ObjectManager::instance();
			auto& system = NativeAudio::AudioSystem::Instance();
			const auto second = objects.createObject("Replacement listener");
			context.Expect(objects.AddComponent<AudioListenerComponent>(second) != nullptr, "The second real listener must be captured.");
			const bool originalFirst = objects.getUUID(fixture.listener) < objects.getUUID(second);
			const auto selected = originalFirst ? fixture.listener : second;
			const auto replacement = originalFirst ? second : fixture.listener;
			fixture.Place(context, selected, {{10, 0, 0}, {0.0, 0.0, 0.0}});
			fixture.Place(context, replacement, {{10, 0, 0}, {10.0, 0.0, 0.0}});
			fixture.Place(context, fixture.source, {{10, 0, 0}, {2.0, 0.0, 0.0}});
			ExpectTwoMetres(context, fixture.Tick(context), 1.0F);
			objects.GetComponent<AudioListenerComponent>(selected)->eligible = false;
			const auto replaced = fixture.Tick(context);
			context.Expect(replaced.audibility == NativeAudio::AudioAudibility::Audible
				&& std::abs(replaced.gain - 0.7056F) <= 0.0001F && std::abs(replaced.spatial.pan + 1.0F) <= 0.0001F,
				"The next capture must use the newly eligible listener and its world origin.");
			objects.GetComponent<AudioListenerComponent>(selected)->eligible = true;
			(void)fixture.Tick(context);
			const auto oldPublication = system.LatestPlaybackSnapshot();
			const auto oldPacketCount = recordingProvider->Packets().size();
			auto workloads = system.DeclareFrameWorkloads(0.0F);
			const auto declaration = system.SchedulerDeclaration();
			const auto contains = [](const auto& resources, const std::string& name) { return std::find(resources.begin(), resources.end(), name) != resources.end(); };
			context.Expect(declaration.phase == "Audio" && declaration.requiresFlushedSceneMutations
				&& contains(declaration.reads, "Canonical world poses") && contains(declaration.reads, "Audio listeners")
				&& contains(declaration.reads, "Audio clip generations"), "Audio diagnostics must declare canonical pose, listener and retained clip-generation reads.");
			context.Expect(workloads.size() == 3U && !workloads[0].workerEligible && workloads[1].workerEligible
				&& !workloads[1].mainThreadRequired && workloads[2].mainThreadRequired,
				"Capture and provider publication are protected while immutable preparation can run on a worker.");
			Scheduling::JobGraphContext jobContext;
			workloads[0].work(jobContext);
			// Change live authoring after capture. Worker preparation must still see
			// the two-metre immutable record, until a later protected capture observes it.
			fixture.Place(context, fixture.source, {{10, 0, 0}, {4.0, 0.0, 0.0}});
			fixture.EnsureWorkerPool();
			// Retain the callback by value so assertion unwinding cannot destroy its
			// workload/context while the fixture joins an outstanding worker.
			auto task = ThreadPool::instance().createTask([prepare = workloads[1].work] {
				Scheduling::JobGraphContext workerContext; prepare(workerContext);
			});
			auto future = task->getFuture(); ThreadPool::instance().enqueue(task);
			context.Expect(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Immutable audio preparation must settle on the shared worker pool.");
			future.get();
			context.Expect(system.LatestPlaybackSnapshot() == oldPublication && recordingProvider->Packets().size() == oldPacketCount,
				"Snapshot and worker preparation must not publish playback or call the provider.");
			workloads[2].work(jobContext);
			ExpectTwoMetres(context, system.LatestPlaybackSnapshot()->voices.front(), 1.0F);
			context.Expect(recordingProvider->Packets().size() == oldPacketCount + 1U, "Protected commit submits precisely one captured packet.");
			const auto diagnostics = NativeAudio::AcquireAudioDiagnosticsSnapshot();
			context.Expect(diagnostics && diagnostics->scheduler.size() == 3U
				&& diagnostics->scheduler.front().reads.find("canonical world poses") != std::string::npos
				&& diagnostics->scheduler.front().reads.find("clip generations") != std::string::npos,
				"Published scheduler diagnostics must expose the actual canonical capture resources.");
			const auto current = fixture.Tick(context);
			context.Expect(std::abs(current.gain - 0.8464F) <= 0.0001F && std::abs(current.spatial.position.x - 4.0F) <= 0.0001F,
				"The next protected capture must observe the newer canonical source pose.");
		});
		registry.Add("Audio.WorldCellCapture.ExplicitCoordinateModesRejectMixedRecords", [](TestContext& context) {
			NativeAudio::AudioAuthoringSnapshotData data;
			data.coordinateMode = NativeAudio::AudioCoordinateMode::SharedLocalFrame;
			NativeAudio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::GenerateUUID(), 1U, 1U}; source.clipAssetId = TerraEngine::GenerateUUID();
			source.clipGeneration = 1U; source.maximumDistance = 50.0F; source.position = {102.0F, 0.0F, 0.0F};
			NativeAudio::AudioAuthoringListenerRecord listener; listener.objectId = TerraEngine::GenerateUUID(); listener.position = {100.0F, 0.0F, 0.0F};
			data.sources = {source}; data.listeners = {listener};
			std::vector<NativeAudio::AudioPlaybackSeed> seeds{{source.identity, NativeAudio::AudioPlaybackState::Playing}};
			const auto prepared = NativeAudio::PrepareAudioFrame(NativeAudio::AudioAuthoringSnapshot::Create(data), seeds);
			ExpectTwoMetres(context, prepared.voices.front(), 1.0F);
			context.Expect(prepared.listener && prepared.listener->position.x == 0.0F, "Compatible local-only snapshots must also publish the listener at zero.");
			const auto rejects = [](NativeAudio::AudioAuthoringSnapshotData invalid) {
				try { (void)NativeAudio::AudioAuthoringSnapshot::Create(std::move(invalid)); } catch(const std::invalid_argument&) { return true; }
				return false;
			};
			data.sources.front().worldPosition = Scene::WorldPosition{};
			context.Expect(rejects(data), "A local snapshot cannot contain a canonical-world record.");
			data.coordinateMode = NativeAudio::AudioCoordinateMode::CanonicalWorld;
			context.Expect(rejects(data), "A canonical snapshot cannot silently fall back to a local-only listener.");
			data.listeners.front().worldPosition = Scene::WorldPosition{};
			data.sources.front().worldPosition = Scene::WorldPosition{{1, 0, 0}, {-32767.0, 0.0, 0.0}};
			data.listeners.front().worldPosition = Scene::WorldPosition{{0, 0, 0}, {32767.0, 0.0, 0.0}};
			seeds = {{source.identity, NativeAudio::AudioPlaybackState::Playing}};
			ExpectTwoMetres(context, NativeAudio::PrepareAudioFrame(NativeAudio::AudioAuthoringSnapshot::Create(data), seeds).voices.front(), 1.0F);
			data.sources.front().maximumDistance = std::numeric_limits<float>::infinity();
			context.Expect(rejects(data), "Non-finite authored range must be rejected before localization.");
			data.sources.front().maximumDistance = 50.0F;
			data.listeners.front().forward.x = std::numeric_limits<float>::quiet_NaN();
			context.Expect(rejects(data), "Non-finite listener basis must be rejected before worker preparation.");
		});
	}
}
