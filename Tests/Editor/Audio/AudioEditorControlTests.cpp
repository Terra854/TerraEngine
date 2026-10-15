// AudioEditorControlTests.cpp - mixer persistence and automation discovery checks.

#include "TestHarness.h"

#include "Audio/AudioMixConfiguration.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"
#include "Providers/AudioAutomationProvider.h"
#include "ProjectAudioSettingsIO.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct OwnedAudioAutomationProject
		{
			OwnedAudioAutomationProject()
				: root(std::filesystem::temp_directory_path() / ("TerraEngineAudioAutomation-"
					+ TerraEngine::ToHexString(TerraEngine::GenerateUUID())))
			{
				// Claim exactly one fresh UUID-named directory. Never reuse or clear
				// a caller's project if an unexpected collision already exists.
				if (!std::filesystem::create_directory(root)) throw std::runtime_error("Audio automation project path already exists.");
			}
			~OwnedAudioAutomationProject()
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}
			std::filesystem::path root;
		};

		// Each case owns a genuine protected ECS capture and the real automation
		// service. A missing test provider keeps device availability separate from
		// logical control correctness; no manual authoring snapshot is injected.
		struct CapturedAutomationAudio
		{
			CapturedAutomationAudio()
			{
				editorContext.projectManager = std::make_shared<ProjectManager>();
				std::string error;
				if (!editorContext.projectManager->CreateProject(project.root, "Audio Control Automation", ProjectTemplate::Blank, &error))
					throw std::runtime_error("Audio automation project setup failed: " + error);
				// Attach only this service's context. GlobalEditorContext remains
				// untouched, preserving other tests' editor ownership.
				service.SetEditorContext(&editorContext);
				audio.Shutdown(); objects.clear();
				ComponentRegistry::RegisterAllComponents();
				const Object object = objects.createObject("Automation Audio Source");
				auto* source = objects.AddComponent<AudioSourceComponent>(object);
				(void)objects.AddComponent<Transform>(object);
				source->playOnStart = true; source->spatialBlend = 0.0F;
				objectId = TerraEngine::ToHexString(objects.getUUID(object));
				audio.SetConfiguredProvider("test.missing");
				audio.SetRuntimePlaybackEnabled(true); audio.Initialize(); audio.Update(0.01F);
			}
			~CapturedAutomationAudio()
			{
				audio.Shutdown(); audio.SetConfiguredProvider("terra.miniaudio"); objects.clear();
				service.SetEditorContext(nullptr);
			}
			nlohmann::json Control(std::string kind, std::string requestId, nlohmann::json extra = nlohmann::json::object())
			{
				extra["objectId"] = objectId; extra["kind"] = std::move(kind);
				return service.HandleTool("audio.control", extra,
					TerraEngine::Editor::Automation::WriteMode::WritesEnabled, requestId);
			}
			TerraEngine::Audio::AudioSystem& audio = TerraEngine::Audio::AudioSystem::Instance();
			ObjectManager& objects = ObjectManager::instance();
			// Declaration order keeps the context/project alive through service
			// destruction, then removes only the owned disposable directory.
			OwnedAudioAutomationProject project;
			EditorContext editorContext;
			TerraEngine::Editor::Automation::EditorAutomationService service;
			std::string objectId;
		};
	}

	void RegisterAudioEditorControlTests(TestRegistry& registry)
	{
		registry.Add("Editor.AudioControl.AutomationPlayAllocatesFreshEngineIdentity", [](TestContext& context) {
			CapturedAutomationAudio fixture;
			const auto before = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(before && before->voices.size() == 1U, "A genuine captured autoplay source must precede automation Play.");
			if (!before || before->voices.empty()) return;
			const auto automatic = before->voices.front().identity.playbackGeneration;
			const auto play = fixture.Control("play", "audio-play-first");
			context.Expect(play.value("ok", false), "The production audio.control operation must admit Play without a caller-owned identity.");
			if (!play.value("ok", false)) return;
			const auto first = play["data"].value("resolvedPlaybackGeneration", std::uint64_t{0U});
			context.Expect(first != 0U && first != automatic, "Play must return a fresh, nonzero engine-owned generation.");
			fixture.audio.Update(0.01F);
			const auto next = fixture.Control("play", "audio-play-second");
			context.Expect(next.value("ok", false) && next["data"].value("resolvedPlaybackGeneration", std::uint64_t{0U}) > first,
				"Another automation Play must allocate a newer identity rather than reuse the wrapper or autoplay ID.");
		});

		registry.Add("Editor.AudioControl.AutomationOmittedGenerationPausesAutoplay", [](TestContext& context) {
			CapturedAutomationAudio fixture;
			const auto before = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(before && before->voices.size() == 1U, "Capture must publish the automatically started source before control admission.");
			if (!before || before->voices.empty()) return;
			const auto automatic = before->voices.front().identity.playbackGeneration;
			const auto pause = fixture.Control("pause", "audio-bind-current");
			context.Expect(pause.value("ok", false) && pause["data"].value("resolvedPlaybackGeneration", std::uint64_t{0U}) == automatic,
				"Omitting playbackGeneration must freeze the captured autoplay instance at admission.");
			context.Expect(!fixture.audio.LatestPlaybackSnapshot()->voices.front().paused,
				"Admitting an automation request must not expose its mutation before AudioCommit.");
			fixture.audio.Update(0.01F);
			const auto after = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(after && !after->voices.empty() && after->voices.front().paused
				&& after->voices.front().identity.playbackGeneration == automatic,
				"The protected audio boundary must pause exactly the instance returned at admission.");
		});

		registry.Add("Editor.AudioControl.AutomationExplicitOldGenerationRejectsWithoutRebinding", [](TestContext& context) {
			CapturedAutomationAudio fixture;
			const auto before = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(before && before->voices.size() == 1U, "A real autoplay instance must exist for the stale exact-generation case.");
			if (!before || before->voices.empty()) return;
			const auto old = before->voices.front().identity.playbackGeneration;
			const auto play = fixture.Control("play", "audio-replacement-play");
			context.Expect(play.value("ok", false), "Replacement Play must be admitted before testing an older exact control.");
			if (!play.value("ok", false)) return;
			const auto replacement = play["data"].value("resolvedPlaybackGeneration", std::uint64_t{0U});
			fixture.audio.Update(0.01F);
			const auto pause = fixture.Control("pause", "audio-exact-old", {{"playbackGeneration", old}});
			fixture.audio.Update(0.01F);
			// Exact controls may reject immediately or at the protected revalidation
			// boundary. Neither outcome may silently control the replacement Play.
			if (pause.value("ok", false)) {
				const auto receipt = fixture.audio.ReadControlReceipt(pause["data"].value("batchSequence", std::uint64_t{0U}));
				context.Expect(receipt && receipt->status == TerraEngine::Audio::AudioControlStatus::Rejected
					&& receipt->rejection == TerraEngine::Audio::AudioControlRejection::StaleTarget,
					"An admitted old exact instance must report stale target at protected revalidation.");
			}
			const auto after = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(after && !after->voices.empty() && !after->voices.front().paused
				&& after->voices.front().identity.playbackGeneration == replacement,
				"A stale explicit instance must leave replacement playback running and unchanged.");
		});

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
			const auto control = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) { return capability.descriptor.name == "audio.control"; });
			context.Expect(capabilities.size() == 3U && mix != capabilities.end() && inspect != capabilities.end() && control != capabilities.end()
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
