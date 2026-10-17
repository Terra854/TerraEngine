// Installed production-helper cost fixture. Build/run instructions are in
// Tests/Tools/CodeQuality/README.md. This deliberately measures
// synchronous call latency, not queue latency, task-pool throughput or GPU work.
#include <Windows.h>
#include <bcrypt.h>
#include "Renderer/SyntheticCapture.h"
#include "Renderer/RenderCaptureEncoding.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using Json = nlohmann::json;
	using Path = std::filesystem::path;
	using Clock = std::chrono::steady_clock;
	using namespace TerraEngine::Renderer;
	constexpr std::size_t kWarmupCount = 3;
	constexpr std::size_t kMeasuredCount = 30;
	constexpr std::uint64_t kFrameId = 1;

	void Require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}

	// Record the actual mapped module, rather than assuming the working directory
	// or executable search path supplied the requested installed DLL.
	Path ModulePath(HMODULE module)
	{
		std::array<wchar_t, 32768> buffer{};
		const DWORD count = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
		Require(count != 0 && count < buffer.size(), "Cannot identify loaded module path.");
		return Path(buffer.data());
	}

	// Windows' bundled SHA-256 provider pins host/DLL bytes outside timed calls.
	// RAII releases hash/provider handles on every failure path.
	Json FilePin(const Path& path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		Require(input.good(), "Cannot open module for SHA-256 pinning.");
		const auto length = input.tellg();
		Require(length >= 0, "Cannot size module for SHA-256 pinning.");
		input.seekg(0);
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot open SHA-256 provider.");
		struct AlgorithmOwner { BCRYPT_ALG_HANDLE value; ~AlgorithmOwner() { BCryptCloseAlgorithmProvider(value, 0); } } algorithmOwner{algorithm};
		BCRYPT_HASH_HANDLE hash = nullptr;
		Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "Cannot create SHA-256 hash.");
		struct HashOwner { BCRYPT_HASH_HANDLE value; ~HashOwner() { BCryptDestroyHash(value); } } hashOwner{hash};
		std::array<char, 65536> buffer{};
		while(input)
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const auto count = input.gcount();
			if(count != 0) Require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) >= 0, "Cannot hash module bytes.");
		}
		Require(input.eof(), "Module hash read failed.");
		std::array<unsigned char, 32> digest{};
		Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "Cannot finish module hash.");
		constexpr char digits[] = "0123456789abcdef";
		std::string text;
		for(const auto byte : digest) { text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]); }
		return {{"path", std::filesystem::canonical(path).generic_string()}, {"sha256", text}, {"bytes", static_cast<std::uint64_t>(length)}};
	}

	struct Options
	{
		Path output, expectedEnginePath;
		std::string expectedEngineSha256;
	};

	Options ParseOptions(int argc, wchar_t** argv)
	{
		Require(argc == 7, "Required: --output <fresh-absolute-json> --expected-engine-path <absolute-dll> --expected-engine-sha256 <sha256>.");
		std::map<std::wstring, std::wstring> values;
		for(int index = 1; index < argc; index += 2)
		{
			const std::wstring key = argv[index];
			Require(key == L"--output" || key == L"--expected-engine-path" || key == L"--expected-engine-sha256", "Unknown command-line option.");
			Require(values.emplace(key, argv[index + 1]).second, "Duplicate command-line option.");
		}
		Require(values.size() == 3, "Missing required command-line option.");
		Options options;
		options.output = Path(values.at(L"--output"));
		options.expectedEnginePath = Path(values.at(L"--expected-engine-path"));
		const auto& digest = values.at(L"--expected-engine-sha256");
		Require(digest.size() == 64, "Expected Engine SHA-256 must contain 64 hexadecimal characters.");
		for(wchar_t value : digest)
		{
			Require((value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') || (value >= L'A' && value <= L'F'), "Expected Engine SHA-256 is not hexadecimal.");
			options.expectedEngineSha256.push_back(static_cast<char>(value >= L'A' && value <= L'F' ? value - L'A' + L'a' : value));
		}
		Require(options.output.is_absolute() && options.expectedEnginePath.is_absolute(), "Output and expected Engine paths must be absolute.");
		Require(std::filesystem::is_directory(options.output.parent_path()), "Output parent directory must already exist.");
		Require(!std::filesystem::exists(options.output), "Output path must be fresh.");
		Require(std::filesystem::is_regular_file(options.expectedEnginePath), "Expected Engine DLL must exist.");
		return options;
	}

	// CREATE_NEW rejects both stale outputs and concurrent attempts to replace a
	// receipt. JSON construction and file I/O are outside every timed region.
	void WriteFresh(const Path& path, const Json& value)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "Cannot create fresh output JSON.");
		struct FileOwner { HANDLE value; ~FileOwner() { CloseHandle(value); } } owner{file};
		const std::string bytes = value.dump(2) + "\n";
		std::size_t offset = 0;
		while(offset < bytes.size())
		{
			DWORD written = 0;
			const DWORD count = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - offset, 65536));
			Require(WriteFile(file, bytes.data() + offset, count, &written, nullptr) && written != 0, "Cannot write output JSON.");
			offset += written;
		}
		Require(FlushFileBuffers(file) != FALSE, "Cannot flush output JSON.");
	}

	// Validate each returned result and consume every Base64/metadata byte after
	// timing stops. This digest is consumption evidence, not a PNG decode oracle.
	std::uint64_t ValidateAndConsume(const RenderCaptureRequest& request,
		const RenderCaptureResult& result, const Rgba8PngLayout& layout)
	{
		Require(result.ok && result.errorCategory.empty(), "Production helper returned a failed capture.");
		Require(result.size == request.targetSize && result.mimeType == "image/png", "Capture extent or MIME type differs from request.");
		Require(result.deterministicFallback && !result.backendReadback && !result.pendingAsync && !result.backendTarget, "Capture provenance is not completed synthetic output.");
		Require(!result.overlaysIncluded, "Synthetic result unexpectedly claims overlays.");
		Require(result.base64Data.size() == layout.base64Bytes, "Base64 length differs from production layout preflight.");
		Require(result.requestId == request.requestId && result.sourceKind == request.sourceKind
			&& result.sourceName == request.sourceName && result.freshness == request.freshness && result.frameId == kFrameId,
			"Capture request/source/frame correlation differs.");
		Require(!result.message.empty() && !result.warnings.empty(), "Synthetic provenance explanation is absent.");
		std::uint64_t digest = 14695981039346656037ULL;
		const auto consume = [&](std::string_view value)
		{
			for(const unsigned char byte : value) { digest ^= byte; digest *= 1099511628211ULL; }
		};
		consume(result.base64Data); consume(result.requestId); consume(result.sourceName);
		consume(result.message); consume(result.errorCategory); consume(result.mimeType); consume(result.freshness);
		for(const auto& warning : result.warnings) consume(warning);
		return digest;
	}

	Json RunCase(std::uint32_t width, std::uint32_t height, bool transparent)
	{
		const auto layout = ValidateRgba8PngLayout(width, height);
		RenderCaptureRequest request;
		request.requestId = std::to_string(width) + "x" + std::to_string(height) + (transparent ? "-transparent" : "-opaque");
		request.sourceKind = RenderCaptureSourceKind::CustomCamera;
		request.sourceName = "synthetic-requested-size-cost";
		request.targetSize = {width, height};
		request.transparentBackground = transparent;
		request.includeOverlays = false;
		request.allowBackendFallback = true;
		std::vector<std::uint64_t> warmup, measured, digests;
		warmup.reserve(kWarmupCount); measured.reserve(kMeasuredCount);
		digests.reserve(kWarmupCount + kMeasuredCount);
		for(std::size_t index = 0; index < kWarmupCount + kMeasuredCount; ++index)
		{
			// One direct call isolates the repaired requested-size helper cost. A
			// task pool would introduce queue/admission noise and measure a different
			// contract. Timing includes internal allocations, alpha/PNG/Base64 and
			// result metadata; it excludes caller validation/hash/decode and returned
			// result destruction. No engine initialization or renderer is involved.
			const auto begin = Clock::now();
			auto result = PrepareSyntheticCapture(request, kFrameId);
			const auto end = Clock::now();
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
			Require(elapsed >= 0, "Steady clock produced negative elapsed time.");
			(index < kWarmupCount ? warmup : measured).push_back(static_cast<std::uint64_t>(elapsed));
			digests.push_back(ValidateAndConsume(request, result, layout));
		}
		auto ordered = measured;
		std::sort(ordered.begin(), ordered.end());
		const double median = (static_cast<double>(ordered[14]) + static_cast<double>(ordered[15])) / 2.0;
		// Nearest-rank p95: ceil(0.95 * 30) is sample 29, zero-based index 28.
		return {{"caseId", request.requestId}, {"width", width}, {"height", height}, {"transparentBackground", transparent},
			{"requestedOverlays", false}, {"overlaysIncluded", false}, {"warmupCount", kWarmupCount}, {"measuredCount", kMeasuredCount},
			{"expectedRgbaBytes", layout.rgbaBytes}, {"expectedPngBytes", layout.pngBytes}, {"expectedBase64Bytes", layout.base64Bytes},
			{"warmupRawNanoseconds", warmup}, {"rawNanoseconds", measured}, {"medianNanoseconds", median}, {"p95Nanoseconds", ordered[28]},
			{"percentileMethod", "nearest-rank"}, {"resultByteDigestsFnv1a64", digests}, {"validatedResultCount", digests.size()}, {"status", "Passed"}};
	}
}

int wmain(int argc, wchar_t** argv)
{
	Path output;
	Json receipt = {{"schemaVersion", "synthetic-capture-requested-size-cost-v1"}, {"status", "Failed"},
		{"measurement", "synchronous production PrepareSyntheticCapture call latency"},
		{"comparisonPolicy", "characterization only; no historical 1x1 ratio or performance threshold"},
		{"includes", {"RGBA allocation/zero fill", "opaque alpha fill when requested", "PNG encoding", "Base64 encoding", "helper metadata", "internal temporary destruction"}},
		{"excludes", {"queue/admission/executor", "backend/GPU", "editor/transport", "PNG decode", "caller validation/digest", "returned result destruction"}},
		{"cases", Json::array()}};
	try
	{
		const auto options = ParseOptions(argc, argv);
		output = options.output;
		const auto engineModule = GetModuleHandleW(L"TerraEngine.dll");
		Require(engineModule != nullptr, "Installed TerraEngine.dll is not loaded.");
		const auto enginePath = ModulePath(engineModule);
		Require(std::filesystem::equivalent(enginePath, options.expectedEnginePath), "Loaded Engine DLL differs from trusted expected path.");
		const auto enginePin = FilePin(enginePath);
		Require(enginePin.at("sha256") == options.expectedEngineSha256, "Loaded Engine DLL differs from trusted SHA-256.");
		receipt["host"] = FilePin(ModulePath(nullptr));
		receipt["loadedEngine"] = enginePin;
		receipt["expectedEnginePath"] = options.expectedEnginePath.generic_string();
		receipt["expectedEngineSha256"] = options.expectedEngineSha256;
		receipt["engineHashVerified"] = true;
		receipt["clock"] = {{"steady", Clock::is_steady}, {"periodNumerator", Clock::period::num}, {"periodDenominator", Clock::period::den}};
		for(const auto extent : std::array<std::array<std::uint32_t, 2>, 3>{{{320, 240}, {1280, 720}, {1920, 1080}}})
			for(const bool transparent : {false, true}) receipt["cases"].push_back(RunCase(extent[0], extent[1], transparent));
		Require(FilePin(enginePath) == enginePin && FilePin(ModulePath(nullptr)) == receipt.at("host"), "Host or Engine file changed during characterization.");
		receipt["status"] = "Passed";
		WriteFresh(output, receipt);
		return 0;
	}
	catch(const std::exception& error)
	{
		receipt["status"] = "Failed";
		receipt["error"] = error.what();
		if(!output.empty())
		{
			try { WriteFresh(output, receipt); }
			catch(const std::exception& writeError) { std::cerr << writeError.what() << '\n'; }
		}
		std::cerr << error.what() << '\n';
		return 1;
	}
}
