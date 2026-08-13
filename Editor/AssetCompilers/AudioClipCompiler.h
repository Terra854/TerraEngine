// AudioClipCompiler.h - transactional WAV/Ogg to provider-neutral TEAF cooking.

#pragma once

#include "File Formats/TerraEngineAudioFile.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace TerraEngine::AssetPipeline
{
	enum class AudioClipCompileStatus : std::uint8_t
	{
		Ok,
		MissingSource,
		UnsupportedEncoding,
		SignatureMismatch,
		TruncatedHeader,
		InvalidMetadata,
		ZeroFrames,
		RangeOverflow,
		DecodeProbeFailed,
		Cancelled,
		IoError,
		PublishFailed
	};

	struct AudioClipCompileInput
	{
		std::filesystem::path sourcePath;
		std::filesystem::path cookedPath;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t maximumPayloadBytes = TerraEngine::Audio::kAudioFileMaximumPayloadBytes;
	};

	struct AudioClipCompileReport
	{
		AudioClipCompileStatus status = AudioClipCompileStatus::MissingSource;
		TerraEngine::Audio::AudioFileMetadata metadata;
		std::uint64_t sourceBytes = 0U;
		std::string diagnostic;

		[[nodiscard]] bool Succeeded() const noexcept { return status == AudioClipCompileStatus::Ok; }
	};

	// Builds a complete staging artifact, validates it through the runtime reader,
	// then publishes atomically while preserving a previous valid cooked file.
	[[nodiscard]] AudioClipCompileReport CompileAudioClip(
		const AudioClipCompileInput& input, const std::atomic_bool* cancellationRequested = nullptr);

	[[nodiscard]] std::string_view ToString(AudioClipCompileStatus status) noexcept;
}
