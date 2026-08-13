// AudioExportRuntimeTests.cpp - packaged TEAF playback and failure-boundary coverage.

#include "TestHarness.h"

#include "Audio/Assets/AudioClipAsset.h"
#include "Audio/AudioDiagnostics.h"
#include "Audio/AudioSystem.h"
#include "File Formats/TerraEngineAudioFile.h"
#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "TEPAK/PackWriter.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		class AudioExportWorkspace final
		{
		public:
			AudioExportWorkspace()
				: m_Root(std::filesystem::temp_directory_path() / ("TerraAudioExportRuntime-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
			{
				std::filesystem::create_directories(m_ContentRoot / "Audio");
			}

			~AudioExportWorkspace()
			{
				ContentMount::Instance().Unmount();
				std::error_code ignored;
				std::filesystem::remove_all(m_Root, ignored);
			}

			[[nodiscard]] std::filesystem::path ArchivePath() const { return m_Root / "AudioRuntime.tepak"; }
			[[nodiscard]] std::filesystem::path ContentPath(std::string_view relativePath) const { return m_ContentRoot / std::filesystem::path(relativePath); }
			[[nodiscard]] std::filesystem::path LoosePath(std::string_view relativePath) const { return m_Root / std::filesystem::path(relativePath); }

			void WriteContent(std::string_view relativePath, const std::vector<std::uint8_t>& bytes) const
			{
				const auto path = ContentPath(relativePath);
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}

			void WriteLoose(std::string_view relativePath, const std::vector<std::uint8_t>& bytes) const
			{
				const auto path = LoosePath(relativePath);
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}

			void WriteManifest() const
			{
				const std::vector<std::uint8_t> manifest{'{', '}'};
				WriteContent("game.json", manifest);
			}

			[[nodiscard]] std::filesystem::path CreateArchive() const
			{
				tepak::PackagingRequest request;
				request.inputRoot = m_ContentRoot;
				request.outputPackPath = ArchivePath();
				request.workerCount = 1U;
				request.enableCompression = false;
				request.enableDictionaryTraining = false;
				request.overwriteExisting = true;
				(void)tepak::PackWriter::Create(request);
				return ArchivePath();
			}

		private:
			std::filesystem::path m_Root;
			std::filesystem::path m_ContentRoot = m_Root / "Content";
		};

		std::vector<std::uint8_t> MakeValidAudioFile(TerraEngine::UUID assetId)
		{
			// Runtime loading validates the TEAF envelope before a provider observes
			// payload bytes; the headless fixture therefore needs no audio hardware.
			const std::vector<std::uint8_t> payload{'R', 'I', 'F', 'F'};
			TerraEngine::Audio::AudioFileMetadata metadata;
			metadata.assetId = assetId;
			metadata.encoding = TerraEngine::Audio::AudioEncoding::Wave;
			metadata.channelCount = 1U;
			metadata.sampleRate = 8000U;
			metadata.frameCount = 2U;
			metadata.sourceChecksum = TerraEngine::Audio::ComputeAudioChecksum(payload);
			std::vector<std::uint8_t> encoded;
			return TerraEngine::Audio::SerializeAudioFile(metadata, payload, encoded)
				== TerraEngine::Audio::AudioFileStatus::Ok ? encoded : std::vector<std::uint8_t>{};
		}

		void ResetExportResources()
		{
			// ResourceManager is process-global, so reset it between archive fixtures
			// to prevent one package's immutable generation masking another package.
			auto& resources = ResourceManager::Instance();
			resources.Shutdown();
			resources.ConfigureExportArchive({});
			ContentMount::Instance().Unmount();
		}
	}

	void RegisterAudioExportRuntimeTests(TestRegistry& registry)
	{
		registry.Add("Engine.AudioExportRuntime.PackagedTeaudioLoadsAndProducesPlaybackPlan", [](TestContext& context) {
			AudioExportWorkspace workspace;
			const TerraEngine::UUID clipId{111U};
			workspace.WriteManifest();
			workspace.WriteContent("Audio/effect.teaudio", MakeValidAudioFile(clipId));

			auto& resources = ResourceManager::Instance();
			resources.ConfigureExportArchive(workspace.CreateArchive());
			resources.Initialize();
			const TerraEngine::UUID loadedId = resources.LoadAudioClip("Audio/effect.teaudio");
			const auto clip = resources.GetAudioClip(loadedId);
			context.Expect(loadedId == clipId && clip && clip->AssetId() == clipId && clip->Generation() != 0U,
				"An exported runtime must load its project-owned .teaudio clip from the mounted archive.");

			Audio::AudioAuthoringSnapshotData authored;
			Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::UUID{112U}, 1U, 1U};
			source.clipAssetId = clipId;
			source.clipGeneration = clip ? clip->Generation() : 0U;
			source.clipFrames = clip ? clip->FrameCount() : 0U;
			source.clipSampleRate = clip ? clip->SampleRate() : 0U;
			source.clipChannels = clip ? clip->Channels() : 0U;
			source.clipPayload = clip ? clip->RetainedPayload() : nullptr;
			source.spatial = false;
			authored.sources.push_back(source);

			// A missing test provider is intentional: AudioSystem still creates the
			// immutable logical playback snapshot without opening a real device.
			auto& system = Audio::AudioSystem::Instance();
			system.Shutdown();
			system.SetConfiguredProvider("test.missing");
			system.Initialize();
			system.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(authored));
			Audio::AudioControlBatch play;
			play.requestedFrame = 1U;
			play.commands.push_back({Audio::AudioControlKind::Play, {source.identity.objectId, 1U, 1U, 1U}});
			(void)system.SubmitControlBatch(std::move(play));
			system.Update(0.0F);
			const auto publication = system.LatestPlaybackSnapshot();
			context.Expect(publication && publication->voices.size() == 1U
				&& publication->voices.front().audibility == Audio::AudioAudibility::Audible,
				"A mounted .teaudio payload must remain available to the exported runtime's non-spatial playback plan.");

			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
			ResetExportResources();
		});

		registry.Add("Engine.AudioExportRuntime.MissingOrCorruptPackagedAudioFailsClosed", [](TestContext& context) {
			AudioExportWorkspace missingWorkspace;
			missingWorkspace.WriteManifest();
			// A matching loose file proves export mode never falls back outside TEPAK.
			missingWorkspace.WriteLoose("Audio/missing.teaudio", MakeValidAudioFile(TerraEngine::UUID{113U}));
			auto& resources = ResourceManager::Instance();
			resources.ConfigureExportArchive(missingWorkspace.CreateArchive());
			resources.Initialize();
			const TerraEngine::UUID missingId = resources.LoadAudioClip("Audio/missing.teaudio");
			context.Expect(missingId == TerraEngine::EMPTY_UUID,
				"A missing packaged .teaudio entry must fail closed instead of reading a loose developer copy.");
			ResetExportResources();

			AudioExportWorkspace corruptWorkspace;
			corruptWorkspace.WriteManifest();
			corruptWorkspace.WriteContent("Audio/corrupt.teaudio", {'b', 'a', 'd'});
			resources.ConfigureExportArchive(corruptWorkspace.CreateArchive());
			resources.Initialize();
			const TerraEngine::UUID corruptId = resources.LoadAudioClip("Audio/corrupt.teaudio");
			context.Expect(corruptId == TerraEngine::EMPTY_UUID,
				"A corrupt packaged .teaudio envelope must be rejected before runtime playback can retain it.");
			ResetExportResources();
		});

		registry.Add("Engine.AudioExportRuntime.UnavailableProviderPublishesActionableNotice", [](TestContext& context) {
			auto& system = Audio::AudioSystem::Instance();
			system.Shutdown();
			system.SetConfiguredProvider("test.missing");
			system.Initialize();
			// Diagnostics are published by AudioCommit, not by provider construction.
			system.Update(0.0F);
			const auto diagnostics = Audio::AcquireAudioDiagnosticsSnapshot();
			const bool providerNotice = diagnostics && !diagnostics->failures.empty()
				&& diagnostics->failures.front().category == "provider.initialize"
				&& !diagnostics->failures.front().message.empty();
			context.Expect(providerNotice,
				"An unavailable packaged-runtime provider must publish a non-empty initialization notice instead of failing silently.");
			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
		});
	}
}
