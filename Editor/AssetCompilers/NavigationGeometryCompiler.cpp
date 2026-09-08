// NavigationGeometryCompiler.cpp - checked cook and atomic file publication.

#include "NavigationGeometryCompiler.h"

#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace TerraEngine::Editor {
	namespace {
		std::mutex g_NavigationGeometryPublicationMutex;

		bool IsCancelled(const NavigationGeometryCompileRequest& request) noexcept {
			return request.cancellationRequested != nullptr
				&& request.cancellationRequested->load(std::memory_order_acquire);
		}

		bool AtomicReplace(const std::filesystem::path& staging, const std::filesystem::path& destination) {
#if defined(_WIN32)
			const std::wstring stagedPath = staging.wstring();
			const std::wstring destinationPath = destination.wstring();
			return std::filesystem::exists(destination)
				? ::ReplaceFileW(destinationPath.c_str(), stagedPath.c_str(), nullptr,
					REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
				: ::MoveFileExW(stagedPath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#else
			std::error_code error;
			std::filesystem::rename(staging, destination, error);
			return !error;
#endif
		}
	}

	NavigationGeometryCompileResult CompileNavigationGeometry(const NavigationGeometryCompileRequest& request) {
		NavigationGeometryCompileResult result;
		result.sourceFingerprint = request.sourceFingerprint;
		result.settingsFingerprint = request.settingsFingerprint;
		if(IsCancelled(request)) {
			result.cancelled = true;
			result.reason = "Cancelled";
			return result;
		}
		if(request.sourceFingerprint == 0U || request.settingsFingerprint == 0U) {
			result.reason = "SourceAndSettingsFingerprintsRequired";
			return result;
		}

		Navigation::NavigationGeometryEncodeResult encoded =
			Navigation::EncodeNavigationGeometry(request.geometry);
		if(!encoded.accepted) {
			result.reason = std::move(encoded.reason);
			return result;
		}
		if(IsCancelled(request)) {
			result.cancelled = true;
			result.reason = "Cancelled";
			return result;
		}

		result.accepted = true;
		result.bytes = std::move(encoded.bytes);
		result.checksum = encoded.checksum;
		return result;
	}

	NavigationGeometryCompileResult CompileAndPublishNavigationGeometry(
		const NavigationGeometryPublishRequest& request) {
		NavigationGeometryCompileResult result = CompileNavigationGeometry(request.compile);
		if(!result.accepted || request.outputPath.empty()) {
			if(result.accepted) {
				result.accepted = false;
				result.reason = "OutputPathRequired";
				result.bytes.clear();
			}
			return result;
		}

		const std::filesystem::path stagingPath = request.outputPath.string() + ".navigation-staging";
		std::error_code error;
		std::filesystem::create_directories(request.outputPath.parent_path(), error);
		if(error) {
			result.accepted = false;
			result.reason = "CreateOutputDirectoryFailed";
			result.bytes.clear();
			return result;
		}
		{
			std::ofstream output(stagingPath, std::ios::binary | std::ios::trunc);
			if(!output || (!result.bytes.empty() && !output.write(
				reinterpret_cast<const char*>(result.bytes.data()),
				static_cast<std::streamsize>(result.bytes.size())))) {
				output.close();
				std::filesystem::remove(stagingPath, error);
				result.accepted = false;
				result.reason = "WriteStagingFailed";
				result.bytes.clear();
				return result;
			}
			output.flush();
			if(!output) {
				output.close();
				std::filesystem::remove(stagingPath, error);
				result.accepted = false;
				result.reason = "FlushStagingFailed";
				result.bytes.clear();
				return result;
			}
		}

		const std::vector<std::byte> stagedBytes = [&stagingPath]() {
			std::ifstream input(stagingPath, std::ios::binary);
			const std::vector<char> rawBytes{
				std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			std::vector<std::byte> bytes(rawBytes.size());
			for(std::size_t index = 0U; index < rawBytes.size(); ++index) {
				bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(rawBytes[index]));
			}
			return bytes;
		}();
		if(!Navigation::DecodeNavigationGeometry(stagedBytes).accepted || IsCancelled(request.compile)) {
			std::filesystem::remove(stagingPath, error);
			result.accepted = false;
			result.cancelled = IsCancelled(request.compile);
			result.reason = result.cancelled ? "Cancelled" : "StagingValidationFailed";
			result.bytes.clear();
			return result;
		}

		{
			std::lock_guard<std::mutex> lock(g_NavigationGeometryPublicationMutex);
			if(!AtomicReplace(stagingPath, request.outputPath)) {
				std::filesystem::remove(stagingPath, error);
				result.accepted = false;
				result.reason = "AtomicReplacementFailed";
				result.bytes.clear();
				return result;
			}
		}
		return result;
	}
}
