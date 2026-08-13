// AudioExportTests.cpp - TEAF export staging and archive inclusion coverage.

#include "TestHarness.h"

#include "Editor/Export/TepakPackager.h"
#include "File Formats/TerraEngineAudioFile.h"
#include "TEPAK/PackReader.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class AudioExportWorkspace final
		{
		public:
			AudioExportWorkspace()
				: m_Root(std::filesystem::temp_directory_path() / ("TerraAudioExport-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
			{
				std::filesystem::create_directories(AssetRoot() / "Audio");
				std::filesystem::create_directories(OutputRoot());
			}

			~AudioExportWorkspace()
			{
				std::error_code ignored;
				std::filesystem::remove_all(m_Root, ignored);
			}

			[[nodiscard]] std::filesystem::path AssetRoot() const { return m_Root / "Assets"; }
			[[nodiscard]] std::filesystem::path OutputRoot() const { return m_Root / "Output"; }
			[[nodiscard]] std::filesystem::path ClipPath() const { return AssetRoot() / "Audio" / "effect.teaudio"; }

			void WriteBytes(const std::vector<std::uint8_t>& bytes) const
			{
				std::ofstream output(ClipPath(), std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}

		private:
			std::filesystem::path m_Root;
		};

		std::vector<std::uint8_t> MakeValidAudioFile()
		{
			// Use a minimal opaque WAV payload: providers decode it later, while TEAF
			// verifies stable identity and byte integrity at every content boundary.
			const std::vector<std::uint8_t> payload{'R', 'I', 'F', 'F'};
			TerraEngine::Audio::AudioFileMetadata metadata;
			metadata.assetId = TerraEngine::UUID{91U};
			metadata.encoding = TerraEngine::Audio::AudioEncoding::Wave;
			metadata.channelCount = 1U;
			metadata.sampleRate = 8000U;
			metadata.frameCount = 2U;
			metadata.sourceChecksum = TerraEngine::Audio::ComputeAudioChecksum(payload);
			std::vector<std::uint8_t> encoded;
			const auto status = TerraEngine::Audio::SerializeAudioFile(metadata, payload, encoded);
			return status == TerraEngine::Audio::AudioFileStatus::Ok ? encoded : std::vector<std::uint8_t>{};
		}
	}

	void RegisterAudioExportTests(TestRegistry& registry)
	{
		registry.Add("Editor.AudioExport.ValidClipIsIncludedAndCorruptionIsRejected", [](TestContext& context) {
			AudioExportWorkspace workspace;
			const std::vector<std::uint8_t> validClip = MakeValidAudioFile();
			workspace.WriteBytes(validClip);

			BuildProfile profile;
			profile.compressionPreset = CompressionPreset::None;
			profile.tepakWorkerCount = 1U;
			TepakPackager packager;
			const std::filesystem::path validPack = workspace.OutputRoot() / "audio-valid.tepak";
			const PackageResult packaged = packager.Package(workspace.AssetRoot(), validPack, profile);
			context.Expect(packaged.success && packaged.entryCount == 1U,
				"A valid staged TEAF clip must be admitted to an export archive.");

			if(packaged.success) {
				const tepak::PackReader reader = tepak::PackReader::Open(validPack);
				const tepak::ByteBuffer archived = reader.ReadAll("Audio/effect.teaudio");
				const std::vector<std::uint8_t> archivedBytes(archived.begin(), archived.end());
				context.Expect(archivedBytes == validClip && TerraEngine::Audio::ReadAudioFile(archivedBytes).Succeeded(),
					"TEPAK must preserve the verified TEAF bytes and make the clip addressable by its staged path.");
				context.Expect(reader.GetEntries().size() == 1U && reader.GetEntries().front().assetClass == "audio",
					"Cooked TEAF entries must retain the audio class used by strict runtime resolution.");
				// This workspace contains only a project-owned clip. Export inventory
				// must never add repository validation media merely because audio is enabled.
				const bool shipsFixtureMedia = std::any_of(reader.GetEntries().begin(), reader.GetEntries().end(), [](const auto& entry) {
					return entry.normalizedPath.find("Tests/Fixtures/Audio") != std::string::npos
						|| entry.normalizedPath.find("Kenney") != std::string::npos;
				});
				context.Expect(!shipsFixtureMedia,
					"An audio export must contain only project-selected content, never test fixture media by default.");
			}

			workspace.WriteBytes({'b', 'a', 'd'});
			const PackageResult corrupt = packager.Package(
				workspace.AssetRoot(), workspace.OutputRoot() / "audio-corrupt.tepak", profile);
			context.Expect(!corrupt.success && !corrupt.errorMessage.empty(),
				"A corrupt or truncated staged .teaudio file must fail export before an archive is written.");

			std::error_code ignored;
			std::filesystem::remove(workspace.ClipPath(), ignored);
			// Keep one unrelated runtime file in the inventory because TEPAK rejects
			// entirely empty archives. The assertion below is specifically about the
			// deleted audio entry not leaking forward from the previous package.
			{
				std::ofstream marker(workspace.AssetRoot() / "runtime-marker.txt", std::ios::binary | std::ios::trunc);
				marker << "runtime";
			}
			const PackageResult missing = packager.Package(
				workspace.AssetRoot(), workspace.OutputRoot() / "audio-missing.tepak", profile);
			context.Expect(missing.success && missing.entryCount == 1U,
				"A removed staged clip must not leave a stale archive entry behind during the next export inventory.");
		});
	}
}
