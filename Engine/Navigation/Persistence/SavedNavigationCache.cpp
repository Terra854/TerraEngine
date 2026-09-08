// SavedNavigationCache.cpp - cancellable checked .tenav file publication.

#include "Navigation/Persistence/SavedNavigationCache.h"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace TerraEngine::Navigation {
	SavedNavigationWriteResult WriteSavedNavigationAtomically(
		const SavedNavigationDescriptor& descriptor,
		std::span<const SavedNavigationChunk> chunks) {
		return EncodeSavedNavigation(descriptor, chunks);
	}

	SavedNavigationReadResult ReadSavedNavigation(
		std::span<const std::byte> bytes,
		const SavedNavigationDescriptor& expected) {
		return DecodeSavedNavigation(bytes, expected);
	}

	SavedNavigationWriteResult WriteSavedNavigationCacheAtomically(
		const std::filesystem::path& destination,
		const SavedNavigationDescriptor& descriptor,
		std::span<const SavedNavigationChunk> chunks,
		const std::atomic_bool* cancellation) {
		SavedNavigationWriteResult result = EncodeSavedNavigation(descriptor, chunks);
		if(!result.accepted || destination.empty()) {
			if(destination.empty()) {
				result.accepted = false;
				result.reason = "SavedNavigationMissingDestination";
			}
			return result;
		}
		if(cancellation != nullptr && cancellation->load(std::memory_order_acquire)) {
			result.accepted = false;
			result.reason = "SavedNavigationCancelled";
			return result;
		}

		std::error_code error;
		if(!destination.parent_path().empty())
			std::filesystem::create_directories(destination.parent_path(), error);
		if(error) {
			result.accepted = false;
			result.reason = "SavedNavigationCreateDirectoryFailed";
			return result;
		}
		const std::filesystem::path staged = destination.string() + ".candidate.tmp";
		{
			std::ofstream output(staged, std::ios::binary | std::ios::trunc);
			if(!output) {
				result.accepted = false;
				result.reason = "SavedNavigationOpenCandidateFailed";
				return result;
			}
			output.write(reinterpret_cast<const char*>(result.bytes.data()),
				static_cast<std::streamsize>(result.bytes.size()));
			output.flush();
			if(!output) {
				output.close();
				std::filesystem::remove(staged, error);
				result.accepted = false;
				result.reason = "SavedNavigationWriteCandidateFailed";
				return result;
			}
		}
		if(cancellation != nullptr && cancellation->load(std::memory_order_acquire)) {
			std::filesystem::remove(staged, error);
			result.accepted = false;
			result.reason = "SavedNavigationCancelled";
			return result;
		}

		bool published = false;
#ifdef _WIN32
		const std::wstring stagedPath = staged.wstring();
		const std::wstring destinationPath = destination.wstring();
		published = std::filesystem::exists(destination)
			? ::ReplaceFileW(destinationPath.c_str(), stagedPath.c_str(), nullptr,
				REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
			: ::MoveFileExW(stagedPath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#else
		std::filesystem::rename(staged, destination, error);
		published = !error;
#endif
		if(!published) {
			std::filesystem::remove(staged, error);
			result.accepted = false;
			result.reason = "SavedNavigationAtomicReplaceFailed";
		}
		return result;
	}

	SavedNavigationReadResult ReadSavedNavigationCache(
		const std::filesystem::path& source,
		const SavedNavigationDescriptor& expected) {
		SavedNavigationReadResult result{};
		std::error_code error;
		const std::uintmax_t fileBytes = std::filesystem::file_size(source, error);
		if(error || fileBytes > kMaximumSavedNavigationBytes) {
			result.reason = error ? "SavedNavigationOpenFailed" : "SavedNavigationCapacityExceeded";
			return result;
		}
		std::vector<std::byte> bytes(static_cast<std::size_t>(fileBytes));
		std::ifstream input(source, std::ios::binary);
		if(!input || !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			result.reason = "SavedNavigationReadFailed";
			return result;
		}
		return DecodeSavedNavigation(bytes, expected);
	}
}
