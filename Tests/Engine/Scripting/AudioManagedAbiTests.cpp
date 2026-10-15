// AudioManagedAbiTests.cpp - fixed-width native v2 bridge and capacity checks.
#include "../TestHarness.h"
#include "Scripting/ScriptGlue.h"
#include "Audio/AudioTypes.h"
#include <cstring>
#include <array>
#include <limits>

namespace TerraEngine::Tests {
	namespace {
		template<class T> T Record() {
			T value{}; value.header = {kNativeAudioAbiVersion, sizeof(T)}; return value;
		}
	}
	void RegisterAudioManagedAbiTests(TestRegistry& registry) {
		registry.Add("Audio.ManagedAbi.VersionAndLayoutsAreFixed", [](TestContext& context) {
			context.ExpectEqual(2u, Native_Audio_GetAbiVersion(), "The managed bridge must report audio ABI v2.");
			context.Expect(sizeof(NativeAudioAbiHeader) == 8 && sizeof(NativeAudioSourceTarget) == 40
				&& sizeof(NativeAudioSourceBinding) == 56 && sizeof(NativeAudioCommandRequest) == 96
				&& sizeof(NativeAudioCommandReceipt) == 240 && sizeof(NativeAudioPlaybackState) == 56
				&& sizeof(NativeAudioMixState) == 32, "Native records must match the managed Pack8 layouts.");
			context.Expect(offsetof(NativeAudioSourceTarget, sceneGeneration) == 16
				&& offsetof(NativeAudioSourceTarget, objectGeneration) == 24
				&& offsetof(NativeAudioSourceTarget, componentGeneration) == 32
				&& offsetof(NativeAudioSourceBinding, target) == 16
				&& offsetof(NativeAudioCommandRequest, target) == 16
				&& offsetof(NativeAudioCommandRequest, playbackGeneration) == 56
				&& offsetof(NativeAudioCommandRequest, idempotencyKey) == 88
				&& offsetof(NativeAudioCommandReceipt, resolvedPlaybackGeneration) == 40
				&& offsetof(NativeAudioCommandReceipt, diagnostic) == 48
				&& offsetof(NativeAudioPlaybackState, logicalFrame) == 40,
				"Every full-width identity and result field must retain its frozen offset.");
			context.ExpectEqual(2u, Audio::kAudioProviderContractVersion,
				"Managed ABI migration must not change the provider packet contract.");
		});
		registry.Add("Audio.ManagedAbi.RejectsV1AndMalformedRequestBeforePayload", [](TestContext& context) {
			// Poisoned payloads must remain unread when the envelope does not match.
			for(const auto header : {NativeAudioAbiHeader{1, 56}, NativeAudioAbiHeader{2, 8}, NativeAudioAbiHeader{2, 95}, NativeAudioAbiHeader{2, 97}}) {
				auto request = Record<NativeAudioCommandRequest>(); request.header = header;
				request.kind = static_cast<NativeAudioCommandKind>(~0u);
				auto receipt = Record<NativeAudioCommandReceipt>();
				context.Expect(Native_Audio_SubmitCommand(&request, &receipt) == NativeAudioStatus::VersionMismatch
					&& receipt.status == NativeAudioStatus::VersionMismatch && receipt.batchSequence == 0,
					"Invalid envelopes must fail before malformed command payloads can be inspected.");
			}
		});
		registry.Add("Audio.ManagedAbi.InvalidOutputCapacityIsNeverWritten", [](TestContext& context) {
			for(const auto header : {NativeAudioAbiHeader{1, 232}, NativeAudioAbiHeader{2, 8}}) {
				auto receipt = Record<NativeAudioCommandReceipt>(); receipt.header = header;
				receipt.batchSequence = std::numeric_limits<std::uint64_t>::max();
				const auto before = receipt;
				context.Expect(Native_Audio_SubmitCommand(nullptr, &receipt) == NativeAudioStatus::VersionMismatch
					&& std::memcmp(&before, &receipt, sizeof(receipt)) == 0,
					"An incompatible output envelope must remain byte-for-byte untouched.");
			}
			context.Expect(Native_Audio_SubmitCommand(nullptr, nullptr) == NativeAudioStatus::VersionMismatch,
				"A null output must be rejected before reading a null request.");
		});
		registry.Add("Audio.ManagedAbi.BindingAndPlaybackGuardBothEnvelopes", [](TestContext& context) {
			auto binding = Record<NativeAudioSourceBinding>(); binding.header.abiVersion = 1;
			const auto before = binding;
			context.Expect(Native_Audio_BindSource(0, &binding) == NativeAudioStatus::VersionMismatch
				&& std::memcmp(&before, &binding, sizeof(binding)) == 0,
				"Binding output must validate its envelope before touching ECS or output data.");
			auto state = Record<NativeAudioPlaybackState>();
			context.Expect(Native_Audio_ReadPlaybackV2(&binding, &state) == NativeAudioStatus::VersionMismatch,
				"A v1 source handle must not be reinterpreted as a full attachment.");
			state.header.structSize = 8; state.logicalFrame = 123;
			const auto oldState = state;
			context.Expect(Native_Audio_ReadPlaybackV2(nullptr, &state) == NativeAudioStatus::VersionMismatch
				&& std::memcmp(&oldState, &state, sizeof(state)) == 0,
				"Playback must reject a short output before reading a binding payload.");
		});
		registry.Add("Audio.ManagedAbi.LegacyPlaybackSignatureRejectsWithoutTouchingStorage", [](TestContext& context) {
			// Model the exact old output capacity and adjacent canaries. Integer
			// entity/generation arguments must never be dereferenced as v2 pointers.
			std::array<unsigned char, 64> legacy{};
			legacy.fill(0xa5);
			const auto before = legacy;
			context.Expect(Native_Audio_ReadPlayback(17u, 1u, legacy.data() + 8) == NativeAudioStatus::VersionMismatch
				&& legacy == before,
				"Legacy playback calls must leave the old 48-byte output and both canaries untouched.");
			context.Expect(Native_Audio_ReadPlayback(0u, 0u, nullptr) == NativeAudioStatus::VersionMismatch,
				"Legacy playback rejection must not dereference a null output.");
		});
		registry.Add("Audio.ManagedAbi.RejectsInvalidCommandsAndModes", [](TestContext& context) {
			auto request = Record<NativeAudioCommandRequest>();
			auto receipt = Record<NativeAudioCommandReceipt>();
			request.kind = static_cast<NativeAudioCommandKind>(~0u);
			context.Expect(Native_Audio_SubmitCommand(&request, &receipt) == NativeAudioStatus::InvalidArgument,
				"Unknown command values must not reach audio admission.");
			request.kind = NativeAudioCommandKind::Pause;
			request.targetMode = static_cast<NativeAudioTargetMode>(~0u);
			context.Expect(Native_Audio_SubmitCommand(&request, &receipt) == NativeAudioStatus::InvalidArgument,
				"Unknown binding modes must not acquire a playback instance.");
			request.targetMode = NativeAudioTargetMode::ExactInstance;
			context.Expect(Native_Audio_SubmitCommand(&request, &receipt) == NativeAudioStatus::InvalidArgument,
				"An empty attachment cannot be submitted as a live source.");
		});
		registry.Add("Audio.ManagedAbi.MixEnvelopeRemainsVersioned", [](TestContext& context) {
			auto mix = Record<NativeAudioMixState>(); mix.header.abiVersion = 1; mix.generation = 123;
			const auto before = mix;
			context.Expect(Native_Audio_ReadMasterMix(&mix) == NativeAudioStatus::VersionMismatch
				&& Native_Audio_SetMasterMix(&mix) == NativeAudioStatus::VersionMismatch
				&& std::memcmp(&before, &mix, sizeof(mix)) == 0,
				"Both master-mix directions must reject v1 without modifying caller storage.");
		});
	}
}
