// AudioFailureRecoveryTests.cpp - bounded provider failures stay deterministic and safe.

#include "../TestHarness.h"
#include "Audio/AudioSystem.h"
#include "FakeAudioBackend.h"

#include <array>

namespace TerraEngine::Tests {
	void RegisterAudioFailureRecoveryTests(TestRegistry& registry) {
		registry.Add("Audio.FailureRecovery.MissingProviderCoalescesIntoSafeSilence", [](TestContext& context) {
			auto& system = TerraEngine::Audio::AudioSystem::Instance();
			system.Shutdown();
			system.SetConfiguredProvider("test.missing");
			system.Initialize();
			for(std::uint32_t frame = 0U; frame < 8U; ++frame) {
				system.Update(1.0F / 60.0F);
			}
			const auto playback = system.LatestPlaybackSnapshot();
			const auto diagnostics = TerraEngine::Audio::AcquireAudioDiagnosticsSnapshot();
			context.Expect(playback && playback->voices.empty(),
				"An unavailable provider must keep the runtime alive with safely silent playback.");
			context.Expect(diagnostics && diagnostics->failures.size() == 1U
				&& diagnostics->failures[0].category == "provider.initialize"
				&& diagnostics->failures[0].count == 1U,
				"A continuing unavailable-provider state must remain one bounded diagnostic episode instead of flooding readers.");
			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
		});

		registry.Add("Audio.FailureRecovery.ProviderFailureDoesNotAdvancePacketSequence", [](TestContext& context) {
			Audio::FakeAudioBackend backend; context.Expect(backend.Initialize({}).Succeeded(), "The fake provider should initialize for deterministic failure injection.");
			backend.FailNextSubmit("simulated device failure");
			TerraEngine::Audio::AudioBackendFramePacket first; first.sequence = 1U; first.requestRecovery = true;
			context.Expect(!backend.SubmitFrame(first).Succeeded(), "The injected provider submit failure should be observable.");
			context.Expect(backend.Packets().empty(), "A failed provider submission must not partially publish a packet.");
			context.Expect(backend.SubmitFrame(first).Succeeded(), "Retrying the same complete reconciliation packet should recover safely.");
			context.Expect(backend.Packets().size() == 1U, "Only the successful packet should be retained as provider evidence.");
			context.Expect(backend.Packets().front().requestRecovery,
				"Retrying after a device failure must preserve the caller's complete recovery reconciliation request.");
		});

		registry.Add("Audio.FailureRecovery.ObservationOverflowIsBounded", [](TestContext& context) {
			Audio::FakeAudioBackend backend; (void)backend.Initialize({});
			for(std::uint32_t index = 0U; index < 40U; ++index) backend.InjectObservation({TerraEngine::Audio::AudioBackendObservationKind::DeviceLost, index, {}, "loss"});
			std::array<TerraEngine::Audio::AudioBackendObservation, 32U> destination{}; std::size_t required = 0U;
			context.Expect(backend.PollObservations(destination, required).Succeeded(), "A sufficiently sized fixed observation buffer should drain without allocation.");
			context.Expect(required == destination.size() && backend.DroppedObservationCount() == 8U,
				"The fixed-capacity channel must retain its bounded prefix and count overflow rather than growing unbounded.");
		});

		registry.Add("Audio.FailureRecovery.DeviceLossAndRecoveryObservationsKeepOrder", [](TestContext& context) {
			Audio::FakeAudioBackend backend;
			(void)backend.Initialize({});
			backend.InjectObservation({TerraEngine::Audio::AudioBackendObservationKind::DeviceLost,
				12U, {}, "device removed"});
			backend.InjectObservation({TerraEngine::Audio::AudioBackendObservationKind::DeviceRecovered,
				13U, {}, "device restored"});
			std::array<TerraEngine::Audio::AudioBackendObservation, 2U> observations{};
			std::size_t required = 0U;
			context.Expect(backend.PollObservations(observations, required).Succeeded() && required == 2U,
				"The provider-neutral observation channel should publish the complete bounded recovery episode.");
			context.Expect(observations[0].kind == TerraEngine::Audio::AudioBackendObservationKind::DeviceLost
				&& observations[1].kind == TerraEngine::Audio::AudioBackendObservationKind::DeviceRecovered,
				"Device loss and recovery must retain callback order for protected-boundary reconciliation.");
		});

		registry.Add("Audio.FailureRecovery.ShutdownContainsVoicesAndObservations", [](TestContext& context) {
			Audio::FakeAudioBackend backend; (void)backend.Initialize({});
			TerraEngine::Audio::AudioBackendFramePacket packet; packet.sequence = 1U; packet.voices.push_back({1U, {}, {}, 0U, 0U, 1.0F, 1.0F, true, false, false, false, {}});
			(void)backend.SubmitFrame(packet); backend.InjectObservation({TerraEngine::Audio::AudioBackendObservationKind::DeviceLost, 1U, {}, "loss"});
			context.Expect(backend.Shutdown().Succeeded() && backend.IsShutdown(), "Provider shutdown should be idempotent and enter a contained state.");
			context.Expect(backend.LiveVoiceCount() == 0U, "Safe-silent shutdown must release every retained provider voice.");
			std::array<TerraEngine::Audio::AudioBackendObservation, 1U> observation{}; std::size_t required = 0U;
			context.Expect(backend.PollObservations(observation, required).Succeeded() && required == 0U,
				"No callback-originated observations may survive provider shutdown.");
		});
	}
}
