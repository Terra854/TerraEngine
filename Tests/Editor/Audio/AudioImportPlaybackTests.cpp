// AudioImportPlaybackTests.cpp - editor cooker publication and reimport retention checks.

#include "TestHarness.h"

#include "Audio/AudioComponentCapture.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Editor/AssetCompilers/AudioClipCompiler.h"
#include "File Formats/TerraEngineAudioFile.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		void WriteU16(std::ofstream& output, std::uint16_t value) { output.put(static_cast<char>(value)); output.put(static_cast<char>(value >> 8U)); }
		void WriteU32(std::ofstream& output, std::uint32_t value) { for(std::uint32_t shift = 0U; shift < 32U; shift += 8U) output.put(static_cast<char>(value >> shift)); }

		void WriteValidWave(const std::filesystem::path& path)
		{
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write("RIFF", 4); WriteU32(output, 40U); output.write("WAVEfmt ", 8); WriteU32(output, 16U);
			WriteU16(output, 1U); WriteU16(output, 1U); WriteU32(output, 8000U); WriteU32(output, 16000U); WriteU16(output, 2U); WriteU16(output, 16U);
			output.write("data", 4); WriteU32(output, 4U); WriteU16(output, 0U); WriteU16(output, 0U);
		}

		std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			// Parentheses select vector's iterator-range constructor instead of an
			// initializer-list conversion, preserving arbitrary binary TEAF bytes.
			return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
		}
	}

	void RegisterAudioImportPlaybackTests(TestRegistry& registry)
	{
		registry.Add("Editor.AudioImportPlayback.ImportPublishesAndFailedReimportRetainsGeneration", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerraAudioImport-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root);
			const auto source = root / "effect.wav";
			const auto cooked = root / "effect.teaudio";
			const TerraEngine::UUID assetId{31U};
			WriteValidWave(source);
			const auto initial = TerraEngine::AssetPipeline::CompileAudioClip({source, cooked, assetId});
			const std::vector<std::uint8_t> knownGood = ReadBytes(cooked);
			const auto validated = TerraEngine::Audio::ReadAudioFile(knownGood);
			context.Expect(initial.Succeeded() && validated.Succeeded() && validated.metadata.assetId == assetId,
				"Audio import must publish a checked TEAF artifact with the .meta-owned UUID.");

			std::ofstream corruptSource(source, std::ios::binary | std::ios::trunc);
			corruptSource << "not-a-wave";
			corruptSource.close();
			const auto rejected = TerraEngine::AssetPipeline::CompileAudioClip({source, cooked, assetId});
			context.Expect(!rejected.Succeeded() && ReadBytes(cooked) == knownGood,
				"A failed watched reimport must retain the complete previous cooked generation.");
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.AudioImportPlayback.InspectorAssignmentPublishesBeforePlayModeCapture", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerraAudioInspector-"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root);
			const auto sourcePath = root / "assigned.wav";
			const auto cookedPath = root / "assigned.teaudio";
			const TerraEngine::UUID clipId{32U};
			WriteValidWave(sourcePath);
			const auto imported = TerraEngine::AssetPipeline::CompileAudioClip({sourcePath, cookedPath, clipId});

			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			Object sourceObject = objects.createObject("Inspector Assigned Audio Source");
			auto* source = objects.AddComponent<AudioSourceComponent>(sourceObject);
			(void)objects.AddComponent<Transform>(sourceObject);
			source->playOnStart = true;
			const TerraEngine::UUID sourceId = objects.getUUID(sourceObject);

			TerraEngine::Scene::TransactionalMutationBuilder assignment(
				"audio-inspector-clip-assignment", TerraEngine::Scene::TransactionOrigin::Editor, "Audio Inspector");
			assignment.ObserveScene(objects).ObserveObject(objects, sourceId)
				.UpdateComponentProperty(sourceId, "AudioSourceComponent", "clipAssetId", TerraEngine::Reflection::Variant{clipId});
			queue.EnqueueTransaction(assignment.Build());
			context.Expect(source->clipAssetId == TerraEngine::EMPTY_UUID,
				"Inspector clip assignment must stay invisible until the declared protected scene flush.");
			const auto applied = queue.Apply(objects, "audio-inspector-assignment");

			// Play Mode receives a copied authoring record, so it observes the
			// committed Inspector value without retaining an editor component pointer.
			const auto captured = TerraEngine::Audio::CaptureAudioSourceAuthoring(
				*source, {sourceId, 1U, 1U}, 1U, 2U, {});
			context.Expect(imported.Succeeded() && applied.applied && source->clipAssetId == clipId
				&& captured.clipAssetId == clipId && captured.enabled,
				"A cooked clip assigned by the Inspector must become the value captured for Play Mode after the flush boundary.");

			// Enter and leave the headless Play Mode lifecycle through AudioSystem.
			// The clip is intentionally not loaded into this isolated fixture, proving
			// an assigned-but-pending source still has safe start/removal behavior.
			auto& system = TerraEngine::Audio::AudioSystem::Instance();
			system.Shutdown();
			system.SetConfiguredProvider("test.missing");
			system.Initialize();
			system.Update(0.0F);
			const auto playPublication = system.LatestPlaybackSnapshot();
			context.Expect(playPublication && playPublication->voices.size() == 1U,
				"Entering Play Mode must create the assigned source's Play On Start lifecycle record.");
			objects.clear();
			system.Update(0.0F);
			const auto stoppedPublication = system.LatestPlaybackSnapshot();
			context.Expect(stoppedPublication && stoppedPublication->voices.empty(),
				"Leaving Play Mode must remove the source lifecycle record after its protected audio boundary.");
			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");

			queue.Clear();
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		});
	}
}
