// Hashing the install closure is deliberately detached worker IO. File handles
// deny concurrent writes while each file is read; no frame or GPU work is touched.
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyInstallIdentity.h"
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		namespace fs = std::filesystem;
		constexpr std::string_view kSeal = "Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json";
		constexpr std::uint64_t kMaximumFileBytes = 16ULL * 1024 * 1024 * 1024;
		constexpr std::uint64_t kMaximumClosureBytes = 256ULL * 1024 * 1024 * 1024;
		constexpr std::size_t kMaximumSealBytes = 16 * 1024 * 1024, kMaximumFiles = 65536;
		void Require(bool condition, std::string_view reason) { if(!condition) throw std::runtime_error(std::string(reason)); }
		std::wstring Key(const fs::path& path)
		{
			auto value = path.lexically_normal().generic_wstring();
			std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); }); return value;
		}
		std::wstring Native(const fs::path& path)
		{
			auto value = path.native();
			// Extended Windows paths require backslashes even when a portable seal
			// supplied forward slashes in a relative component.
			std::replace(value.begin(), value.end(), L'/', L'\\');
			if(value.starts_with(L"\\\\?\\")) return value;
			return value.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + value.substr(2) : L"\\\\?\\" + value;
		}
		void NoRedirects(const fs::path& path)
		{
			fs::path current;
			for(const auto& component : path)
			{
				current /= component;
				// A drive root arrives as two components (C: then /). Check the
				// complete root, not the transient drive-relative C: spelling.
				if(current.has_root_name() && !current.has_root_directory()) continue;
				const auto attributes = GetFileAttributesW(Native(current).c_str());
				Require(attributes != INVALID_FILE_ATTRIBUTES, "Install identity path is missing or inaccessible.");
				Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Install identity path redirects through a symlink or junction.");
			}
		}
		struct File
		{
			HANDLE value = INVALID_HANDLE_VALUE;
			~File() { if(value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		};
		struct Algorithm
		{
			BCRYPT_ALG_HANDLE value = nullptr;
			~Algorithm() { if(value) BCryptCloseAlgorithmProvider(value, 0); }
		};
		// CNG borrows object storage, so destruction order retains it until the hash
		// is destroyed, then closes the provider. Every instance owns its own state.
		class Sha256
		{
		public:
			Sha256()
			{
				ULONG length = 0, returned = 0;
				Require(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider unavailable.");
				Require(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &returned, 0) >= 0, "SHA256 storage query failed.");
				storage.resize(length);
				Require(BCryptCreateHash(algorithm.value, &hash, storage.data(), length, nullptr, 0, 0) >= 0, "SHA256 initialization failed.");
			}
			~Sha256() { if(hash) BCryptDestroyHash(hash); }
			void Add(const void* bytes, std::size_t count)
			{
				// Parentheses preserve the typed limit when Windows headers define max.
				Require(count <= (std::numeric_limits<ULONG>::max)() && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)), static_cast<ULONG>(count), 0) >= 0, "SHA256 streaming update failed.");
			}
			std::string Finish()
			{
				std::array<unsigned char, 32> digest{};
				Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA256 finalization failed.");
				constexpr char alphabet[] = "0123456789abcdef"; std::string result;
				for(auto byte : digest) { result += alphabet[byte >> 4]; result += alphabet[byte & 15]; } return result;
			}
			Sha256(const Sha256&) = delete;
			Sha256& operator=(const Sha256&) = delete;
		private:
			Algorithm algorithm;
			std::vector<unsigned char> storage;
			BCRYPT_HASH_HANDLE hash = nullptr;
		};
		struct Digest { std::string hash, bytes; std::uint64_t size = 0; };
		Digest HashFile(const fs::path& path, std::uint64_t limit, bool retainBytes = false)
		{
			NoRedirects(path); File file;
			file.value = CreateFileW(Native(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			Require(file.value != INVALID_HANDLE_VALUE, "Cannot open sealed installed file for reading.");
			BY_HANDLE_FILE_INFORMATION information{};
			Require(GetFileInformationByHandle(file.value, &information) && !(information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "Sealed artifact is not a regular nonredirected file.");
			// Resolve the opened handle too: an ancestor must not redirect between the
			// initial path checks and opening the file.
			std::vector<wchar_t> finalPath(32768);
			const auto characters = GetFinalPathNameByHandleW(file.value, finalPath.data(), static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			Require(characters && characters < finalPath.size() && Key(fs::path(std::wstring(finalPath.data(), characters))) == Key(fs::path(Native(path))), "Opened artifact does not match its sealed path.");
			Digest result; result.size = (std::uint64_t(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
			Require(result.size <= limit, "Installed identity file exceeds bounded size.");
			if(retainBytes) result.bytes.reserve(static_cast<std::size_t>(result.size));
			Sha256 hash; std::array<unsigned char, 65536> buffer{}; std::uint64_t observed = 0;
			for(;;)
			{
				DWORD read = 0; Require(ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr), "Installed identity file read failed.");
				if(!read) break;
				observed += read; Require(observed <= result.size, "Installed file size changed while hashing."); hash.Add(buffer.data(), read);
				if(retainBytes) result.bytes.append(reinterpret_cast<const char*>(buffer.data()), read);
			}
			Require(observed == result.size, "Installed file is shorter than its opened size.");
			NoRedirects(path); result.hash = hash.Finish(); return result;
		}
		bool Hex(std::string_view value, std::size_t size)
		{
			return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		}
		fs::path Relative(std::string_view value)
		{
			Require(!value.empty() && value.size() <= 4096 && value.find_first_of("\\:\t\r\n") == std::string_view::npos && value.find('\0') == std::string_view::npos,
				"Seal artifact path must be bounded portable relative text.");
			const auto path = fs::u8path(value.begin(), value.end());
			const auto portable = path.generic_u8string();
			Require(!path.is_absolute() && !path.has_root_path() && std::string_view(reinterpret_cast<const char*>(portable.data()), portable.size()) == value, "Seal artifact path is not a canonical relative path.");
			for(const auto& component : path)
			{
				const auto text = component.native();
				Require(!text.empty() && text != L"." && text != L".." && text.back() != L'.' && text.back() != L' ', "Seal artifact path contains an alias or traversal component.");
			}
			return path;
		}
		std::string Lower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); }); return value;
		}
		bool HasSegment(std::string_view path, std::string_view segment)
		{
			const auto text = "/" + Lower(std::string(path)); return text.find("/" + std::string(segment) + "/") != std::string::npos;
		}
		bool Shader(std::string_view path)
		{
			if(!HasSegment(path, "shaders") && !HasSegment(path, "installedshaders")) return false;
			const auto dot = path.find_last_of('.');
			const auto extension = dot == std::string_view::npos ? std::string{} : Lower(std::string(path.substr(dot)));
			static constexpr std::array<std::string_view, 12> extensions{".spv", ".tspf", ".hlsl", ".hlsli", ".vert", ".frag", ".comp", ".task", ".mesh", ".geom", ".tesc", ".tese"};
			return extension == ".glsl" || std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
		}
		bool Asset(std::string_view path)
		{
			return Lower(std::string(path)).starts_with("tests/fixtures/rendererefficiency/") || HasSegment(path, "assets");
		}
	}

	bool VerifyRendererEfficiencyInstallIdentity(const fs::path& installRoot, RendererEfficiencyInstallIdentity& output, std::string& error)
	{
		try
		{
			const auto absolute = fs::absolute(installRoot).lexically_normal(); NoRedirects(absolute);
			const auto root = fs::canonical(absolute); const auto seal = HashFile(root / kSeal, kMaximumSealBytes, true);
			const auto document = nlohmann::json::parse(seal.bytes);
			Require(document.at("schemaVersion") == 1 && document.at("sourceDirty").is_boolean(), "Installed seal schema/version is invalid.");
			const auto revision = document.at("revision").get<std::string>(); Require(Hex(revision, 40), "Installed seal revision must be a full lowercase commit SHA.");
			const auto& artifacts = document.at("artifacts"); Require(artifacts.is_array() && !artifacts.empty() && artifacts.size() <= kMaximumFiles, "Installed seal artifact count is invalid.");
			struct Row { std::string path, hash; std::uint64_t bytes; };
			std::vector<Row> rows; std::set<std::wstring> unique; std::uint64_t total = 0;
			for(const auto& artifact : artifacts)
			{
				const auto pathText = artifact.at("path").get<std::string>(); const auto relative = Relative(pathText);
				Require(unique.insert(Key(relative)).second && Lower(pathText) != Lower(std::string(kSeal)), "Installed seal contains duplicate or self-referential artifacts.");
				const auto expectedHash = artifact.at("sha256").get<std::string>(); Require(Hex(expectedHash, 64), "Installed seal artifact SHA256 is malformed.");
				const auto& declaredSize = artifact.at("bytes"); Require(declaredSize.is_number_unsigned() || (declaredSize.is_number_integer() && declaredSize.get<std::int64_t>() >= 0), "Installed artifact byte count is invalid.");
				const auto bytes = declaredSize.get<std::uint64_t>(); Require(bytes <= kMaximumFileBytes && total <= kMaximumClosureBytes - bytes, "Installed artifact closure exceeds bounded byte count."); total += bytes;
				const auto actual = HashFile(root / relative, kMaximumFileBytes); Require(actual.size == bytes && actual.hash == expectedHash, "Installed artifact bytes/hash do not match their seal.");
				rows.push_back({pathText, actual.hash, actual.size});
			}
			std::sort(rows.begin(), rows.end(), [](const Row& first, const Row& second) {
				return std::lexicographical_compare(first.path.begin(), first.path.end(), second.path.begin(), second.path.end(), [](unsigned char a, unsigned char b) { return a < b; });
			});
			Sha256 binary, shader, asset; unsigned binaryCount = 0, shaderCount = 0, assetCount = 0;
			for(const auto& row : rows)
			{
				// This exact UTF8 encoding is shared with the installed runner. Use
				// verified actual sizes/hashes, never opaque requested identity strings.
				const auto line = row.path + "\t" + std::to_string(row.bytes) + "\t" + row.hash + "\n";
				if(row.path == "Editor/TerraEngine-Editor.exe" || row.path == "Editor/TerraEngine.dll") { binary.Add(line.data(), line.size()); ++binaryCount; }
				if(Shader(row.path)) { shader.Add(line.data(), line.size()); ++shaderCount; }
				if(Asset(row.path)) { asset.Add(line.data(), line.size()); ++assetCount; }
			}
			Require(binaryCount == 2 && shaderCount && assetCount, "Installed seal lacks the required binary, shader or asset closure.");
			Require(HashFile(root / kSeal, kMaximumSealBytes).hash == seal.hash, "Installed seal changed during identity preparation.");
			RendererEfficiencyInstallIdentity prepared{root, revision, binary.Finish(), shader.Finish(), asset.Finish(), seal.hash};
			output = std::move(prepared); error.clear(); return true;
		}
		catch(const std::exception& failure) { error = failure.what(); return false; }
	}
	bool PrepareRendererEfficiencyInstallIdentity(RendererEfficiencyInstallIdentity& output, std::string& error)
	{
		try
		{
			std::vector<wchar_t> buffer(32768); const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			Require(size && size < buffer.size(), "Running editor executable path is unavailable.");
			const auto executable = fs::absolute(fs::path(std::wstring(buffer.data(), size))).lexically_normal(); NoRedirects(executable);
			const auto root = executable.parent_path().parent_path();
			Require(Key(executable) == Key(root / "Editor/TerraEngine-Editor.exe"), "Install identity must be prepared by the installed editor executable.");
			return VerifyRendererEfficiencyInstallIdentity(root, output, error);
		}
		catch(const std::exception& failure) { error = failure.what(); return false; }
	}
}
