// AudioEditorControlTests.cpp - mixer persistence and automation discovery checks.

#include "TestHarness.h"

#include "Audio/AudioMixConfiguration.h"
#include "Providers/AudioAutomationProvider.h"
#include "ProjectAudioSettingsIO.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	void RegisterAudioEditorControlTests(TestRegistry& registry)
	{
		registry.Add("Editor.AudioControl.MixerSnapshotsValidateAndRetainMasterMute", [](TestContext& context) {
			TerraEngine::Audio::AudioMixConfiguration mix;
			std::string diagnostic;
			const auto initial = mix.Snapshot();
			context.Expect(mix.SetMasterVolume(0.25F, diagnostic) && mix.SetMasterMuted(true),
				"Mixer controls must accept a finite volume and separate master mute state.");
			const auto muted = mix.Snapshot();
			context.Expect(muted.master.volume == 0.25F && muted.master.muted && muted.generation > initial.generation,
				"Master mute must persist independently from the playback timeline or volume value.");
			TerraEngine::Audio::AudioMixSnapshot invalid = muted;
			invalid.groups.push_back({TerraEngine::UUID{1U}, "default", 1.0F, false, 1U});
			context.Expect(!mix.Replace(invalid, diagnostic) && !diagnostic.empty(),
				"Duplicate group names must reject atomically rather than publishing a partial mixer state.");
		});

		registry.Add("Editor.AudioControl.AutomationCapabilitiesDeclareCommitBoundary", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			AudioAutomationProvider provider([](std::string_view operation) {
				return AutomationHandler{[name = std::string(operation)](const AutomationRequestContext&) {
					return AutomationDispatchResult{true, ErrorCategory::Internal, {}, {{"operation", name}}};
				}};
			});
			const auto capabilities = provider.GetCapabilities();
			const auto mix = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) { return capability.descriptor.name == "audio.mix"; });
			const auto inspect = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) { return capability.descriptor.name == "audio.inspect"; });
			context.Expect(capabilities.size() == 3U && mix != capabilities.end() && inspect != capabilities.end()
				&& mix->descriptor.execution.visibilityBoundary == "next_audio_commit"
				&& inspect->descriptor.mutationMode == MutationMode::ReadOnly,
				"Audio automation discovery must expose typed controls and truthful snapshot/commit visibility.");
		});

		registry.Add("Editor.AudioControl.ProjectMixerSettingsRoundTrip", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerraAudioMix-"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			TerraEngine::Audio::AudioMixConfiguration mix;
			std::string diagnostic;
			auto authored = mix.Snapshot();
			authored.master = {0.35F, true, 4U};
			authored.groups.push_back({TerraEngine::UUID{7U}, "Music", 0.65F, false, 2U});
			TerraEngine::Audio::AudioMixSnapshot loaded;
			const bool persisted = TerraEngine::Editor::ProjectSettings::PersistProjectAudioMixSettings(root, authored, diagnostic);
			const bool restored = TerraEngine::Editor::ProjectSettings::ReadProjectAudioMixSettings(root, loaded, diagnostic);
			context.Expect(persisted && restored && loaded.master.muted && loaded.groups.size() == 2U
				&& loaded.groups[1].name == "Music" && loaded.groups[1].volume == 0.65F,
				"Project mixer state must round-trip through the provider-neutral settings document.");

			const TerraEngine::Audio::AudioMixSnapshot knownGood = loaded;
			{
				std::ofstream corrupt(root / TerraEngine::Editor::ProjectSettings::kAudioSettingsRelativePath,
					std::ios::trunc);
				corrupt << "{ not valid JSON";
			}
			const bool corruptRejected = !TerraEngine::Editor::ProjectSettings::ReadProjectAudioMixSettings(
				root, loaded, diagnostic);
			const bool groupsRetained = loaded.groups.size() == knownGood.groups.size()
				&& std::equal(loaded.groups.begin(), loaded.groups.end(), knownGood.groups.begin(),
					[](const TerraEngine::Audio::AudioMixGroupState& current,
						const TerraEngine::Audio::AudioMixGroupState& previous) {
						return current.id == previous.id && current.name == previous.name
							&& current.volume == previous.volume && current.muted == previous.muted;
					});
			context.Expect(corruptRejected && loaded.master.volume == knownGood.master.volume
				&& loaded.master.muted == knownGood.master.muted && groupsRetained,
				"A malformed mixer settings document must fail without replacing the last usable mix snapshot.");
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		});
	}
}
