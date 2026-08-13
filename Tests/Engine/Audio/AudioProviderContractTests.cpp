// AudioProviderContractTests.cpp - provider-neutral registration and packet rules.

#include "../TestHarness.h"
#include "FakeAudioBackend.h"
#include "Audio/Backend/AudioBackendRegistry.h"

namespace TerraEngine::Tests {
	void RegisterAudioProviderContractTests(TestRegistry& registry) {
		registry.Add("Audio.ProviderContract.RegistryFreezesAndRejectsDuplicates", [](TestContext& context) {
			TerraEngine::Audio::Backend::AudioBackendRegistry providers;
			const TerraEngine::Audio::AudioProviderDescriptor descriptor{"test.fake", "Deterministic Test Audio", 1U, "1", "fixture",
				TerraEngine::Audio::kRequiredAudioProviderCapabilities, 64U};
			context.Expect(providers.RegisterProvider(descriptor, [] { return std::make_unique<TerraEngine::Tests::Audio::FakeAudioBackend>(); }).result.Succeeded(),
				"A complete source-level provider should register before initialization.");
			context.Expect(!providers.RegisterProvider(descriptor, [] { return std::make_unique<TerraEngine::Tests::Audio::FakeAudioBackend>(); }).result.Succeeded(),
				"Duplicate provider identifiers must be rejected.");
			providers.Freeze();
			context.Expect(!providers.RegisterProvider({"late", "Late", 1U, "1", "x", descriptor.capabilities, 1U},
				[] { return std::make_unique<TerraEngine::Tests::Audio::FakeAudioBackend>(); }).result.Succeeded(),
				"Provider registration must be immutable after registry freeze.");
		});

		registry.Add("Audio.ProviderContract.PacketOrderAndShutdownAreDeterministic", [](TestContext& context) {
			TerraEngine::Tests::Audio::FakeAudioBackend backend;
			context.Expect(backend.Initialize({}).Succeeded(), "The deterministic provider should initialize.");
			TerraEngine::Audio::AudioBackendFramePacket first; first.sequence = 1U;
			TerraEngine::Audio::AudioBackendFramePacket stale; stale.sequence = 1U;
			context.Expect(backend.SubmitFrame(first).Succeeded(), "The first packet should be accepted.");
			context.Expect(!backend.SubmitFrame(stale).Succeeded(), "A duplicate sequence must fail closed.");
			context.Expect(backend.Shutdown().Succeeded() && backend.Shutdown().Succeeded(),
				"Provider shutdown must be idempotent.");
		});
	}
}
