// NativeAotProjectWorkspace.cpp - private long-path-safe .NET workspaces.

#include "NativeAotProjectWorkspace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#endif

namespace {
	// NativeAOT appends compiler-package and response-file paths before starting
	// legacy child tools. Refuse a temporary root that would erase the deliberate
	// headroom provided by the private workspace.
	constexpr std::size_t kMaximumPrivateWorkspaceRootLength = 120U;
	constexpr std::size_t kWorkspaceCreationAttempts = 32U;

	std::string PathForMessage(const std::filesystem::path& path)
	{
		const std::u8string encoded = path.generic_u8string();
		return std::string(
			reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	std::string BuildUnpredictableWorkspaceName()
	{
		// std::random_device is backed by the operating system on the supported
		// toolchains. Four independent words make accidental or guessed names
		// impractical while create_directory remains the final collision guard.
		std::array<std::uint32_t, 4U> words{};
		std::random_device random;
		for(std::uint32_t& word : words) word = random();

		std::ostringstream name;
		name << "TEAOT-" << std::hex << std::setfill('0');
		for(const std::uint32_t word : words) name << std::setw(8) << word;
		return name.str();
	}

#ifdef _WIN32
	struct NativeAotMountPointReparseData {
		DWORD reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
		WORD reparseDataLength = 0U;
		WORD reserved = 0U;
		WORD substituteNameOffset = 0U;
		WORD substituteNameLength = 0U;
		WORD printNameOffset = 0U;
		WORD printNameLength = 0U;
		wchar_t pathBuffer[1]{};
	};

	static_assert(offsetof(NativeAotMountPointReparseData, pathBuffer) == 16U,
		"The mount-point reparse buffer must match the Windows ABI.");

	std::string WindowsErrorMessage(DWORD error)
	{
		return std::system_category().message(static_cast<int>(error));
	}

	std::wstring LocalAbsoluteTarget(const std::filesystem::path& target)
	{
		std::wstring native = target.native();
		for(wchar_t& character : native) {
			if(character == L'/') character = L'\\';
		}
		if(native.rfind(L"\\\\?\\", 0U) == 0U) native.erase(0U, 4U);
		return native;
	}

	bool CreateDirectoryJunction(
		const std::filesystem::path& alias,
		const std::filesystem::path& target,
		DWORD& outError)
	{
		outError = ERROR_SUCCESS;
		const std::wstring printName = LocalAbsoluteTarget(target);
		// Junctions are local NTFS mount points. Reject UNC/device targets instead
		// of creating an alias whose behavior varies with network policy.
		if(printName.size() < 3U || printName[1U] != L':'
			|| printName[2U] != L'\\') {
			outError = ERROR_NOT_SUPPORTED;
			return false;
		}

		const std::wstring substituteName = L"\\??\\" + printName;
		const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
		const std::size_t printBytes = printName.size() * sizeof(wchar_t);
		const std::size_t pathBytes = substituteBytes + sizeof(wchar_t)
			+ printBytes + sizeof(wchar_t);
		const std::size_t reparseDataLength = 8U + pathBytes;
		const std::size_t bufferSize = 8U + reparseDataLength;
		if(reparseDataLength > std::numeric_limits<WORD>::max()
			|| bufferSize > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
			outError = ERROR_BUFFER_OVERFLOW;
			return false;
		}

		if(!CreateDirectoryW(alias.c_str(), nullptr)) {
			outError = GetLastError();
			return false;
		}

		const HANDLE directory = CreateFileW(
			alias.c_str(),
			GENERIC_WRITE,
			0U,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if(directory == INVALID_HANDLE_VALUE) {
			outError = GetLastError();
			(void)RemoveDirectoryW(alias.c_str());
			return false;
		}

		std::vector<std::byte> buffer(bufferSize);
		auto* reparse =
			reinterpret_cast<NativeAotMountPointReparseData*>(buffer.data());
		reparse->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
		reparse->reparseDataLength = static_cast<WORD>(reparseDataLength);
		reparse->substituteNameOffset = 0U;
		reparse->substituteNameLength = static_cast<WORD>(substituteBytes);
		reparse->printNameOffset =
			static_cast<WORD>(substituteBytes + sizeof(wchar_t));
		reparse->printNameLength = static_cast<WORD>(printBytes);
		std::memcpy(
			reparse->pathBuffer,
			substituteName.c_str(),
			substituteBytes + sizeof(wchar_t));
		std::memcpy(
			reinterpret_cast<std::byte*>(reparse->pathBuffer)
				+ reparse->printNameOffset,
			printName.c_str(),
			printBytes + sizeof(wchar_t));

		DWORD returnedBytes = 0U;
		const BOOL created = DeviceIoControl(
			directory,
			FSCTL_SET_REPARSE_POINT,
			reparse,
			static_cast<DWORD>(bufferSize),
			nullptr,
			0U,
			&returnedBytes,
			nullptr);
		if(!created) outError = GetLastError();
		(void)CloseHandle(directory);
		if(!created) {
			(void)RemoveDirectoryW(alias.c_str());
			return false;
		}
		return true;
	}

	bool IsMountPointJunction(
		const std::filesystem::path& alias,
		DWORD& outError)
	{
		outError = ERROR_SUCCESS;
		const HANDLE directory = CreateFileW(
			alias.c_str(),
			0U,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if(directory == INVALID_HANDLE_VALUE) {
			outError = GetLastError();
			return false;
		}

		std::vector<std::byte> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
		DWORD returnedBytes = 0U;
		const BOOL inspected = DeviceIoControl(
			directory,
			FSCTL_GET_REPARSE_POINT,
			nullptr,
			0U,
			buffer.data(),
			static_cast<DWORD>(buffer.size()),
			&returnedBytes,
			nullptr);
		if(!inspected) outError = GetLastError();
		(void)CloseHandle(directory);
		if(!inspected || returnedBytes < sizeof(DWORD)) return false;

		DWORD tag = 0U;
		std::memcpy(&tag, buffer.data(), sizeof(tag));
		if(tag != IO_REPARSE_TAG_MOUNT_POINT) {
			outError = ERROR_REPARSE_TAG_MISMATCH;
			return false;
		}
		return true;
	}
#endif
}

NativeAotProjectWorkspace::NativeAotProjectWorkspace(
	std::filesystem::path sourceProjectRoot)
	: sourceProjectRoot_(std::move(sourceProjectRoot))
{
}

NativeAotProjectWorkspace::~NativeAotProjectWorkspace()
{
	try {
		std::string ignoredError;
		(void)Cleanup(ignoredError);
	}
	catch(...) {
		// Destructors cannot report cleanup failures. Explicit Cleanup is the
		// reportable path used after successful compilation.
	}
}

bool NativeAotProjectWorkspace::Prepare(std::string& outError)
{
	outError.clear();
	if(!workspaceRoot_.empty() && !Cleanup(outError)) return false;

	std::error_code pathError;
	sourceProjectRoot_ = std::filesystem::absolute(sourceProjectRoot_, pathError)
		.lexically_normal();
	if(pathError || !std::filesystem::is_directory(sourceProjectRoot_, pathError)) {
		outError = "NativeAOT project root is missing or unreadable: "
			+ PathForMessage(sourceProjectRoot_);
		if(pathError) outError += ": " + pathError.message();
		return false;
	}

	const std::filesystem::path temporaryRoot =
		std::filesystem::absolute(
			std::filesystem::temp_directory_path(pathError), pathError)
			.lexically_normal();
	if(pathError) {
		outError = "Could not resolve the temporary directory for the private "
			"NativeAOT workspace: " + pathError.message();
		return false;
	}

	for(std::size_t attempt = 0U;
		attempt < kWorkspaceCreationAttempts;
		++attempt) {
		std::filesystem::path candidate;
		try {
			candidate = temporaryRoot / BuildUnpredictableWorkspaceName();
		}
		catch(const std::exception& exception) {
			outError = "Could not generate an unpredictable NativeAOT workspace "
				"name: " + std::string(exception.what());
			return false;
		}

		if(candidate.native().size() > kMaximumPrivateWorkspaceRootLength) {
			outError = "The temporary directory is too long to host a private "
				"NativeAOT workspace: " + PathForMessage(temporaryRoot);
			return false;
		}

		pathError.clear();
		if(std::filesystem::create_directory(candidate, pathError)) {
			workspaceRoot_ = std::move(candidate);
			break;
		}
		if(pathError
			&& pathError != std::errc::file_exists) {
			outError = "Could not create the private NativeAOT workspace: "
				+ pathError.message();
			return false;
		}
	}

	if(workspaceRoot_.empty()) {
		outError = "Could not allocate a unique private NativeAOT workspace "
			"after " + std::to_string(kWorkspaceCreationAttempts) + " attempts.";
		return false;
	}

	aliasPath_ = workspaceRoot_ / "p";
	intermediateOutputDirectory_ = workspaceRoot_ / "obj";
	binaryOutputDirectory_ = workspaceRoot_ / "bin";
	publishDirectory_ = workspaceRoot_ / "publish";
	packageCacheDirectory_ = workspaceRoot_ / "packages";
	dotnetCliHomeDirectory_ = workspaceRoot_ / "dotnet-home";

	const std::array<std::filesystem::path, 5U> privateDirectories{
		intermediateOutputDirectory_,
		binaryOutputDirectory_,
		publishDirectory_,
		packageCacheDirectory_,
		dotnetCliHomeDirectory_
	};
	for(const std::filesystem::path& directory : privateDirectories) {
		pathError.clear();
		std::filesystem::create_directory(directory, pathError);
		if(pathError) {
			outError = "Could not create NativeAOT workspace directory "
				+ PathForMessage(directory) + ": " + pathError.message();
			std::string ignoredCleanupError;
			(void)Cleanup(ignoredCleanupError);
			return false;
		}
	}

#ifdef _WIN32
	DWORD junctionError = ERROR_SUCCESS;
	if(!CreateDirectoryJunction(aliasPath_, sourceProjectRoot_, junctionError)) {
		outError = "Could not create the private NativeAOT project junction: "
			+ WindowsErrorMessage(junctionError);
		std::string ignoredCleanupError;
		(void)Cleanup(ignoredCleanupError);
		return false;
	}
	buildProjectRoot_ = aliasPath_;

	DWORD tagError = ERROR_SUCCESS;
	std::error_code equivalentError;
	if(!IsMountPointJunction(aliasPath_, tagError)
		|| !std::filesystem::equivalent(
			aliasPath_, sourceProjectRoot_, equivalentError)
		|| equivalentError) {
		outError = "The private NativeAOT project junction did not retain the "
			"required mount-point tag and exact project target";
		if(tagError != ERROR_SUCCESS) {
			outError += ": " + WindowsErrorMessage(tagError);
		}
		std::string ignoredCleanupError;
		(void)Cleanup(ignoredCleanupError);
		return false;
	}
#else
	// POSIX hosts do not need a project junction. Generated paths still live in
	// the private workspace so concurrent builds cannot share obj/bin state.
	aliasPath_.clear();
	buildProjectRoot_ = sourceProjectRoot_;
#endif

	return true;
}

bool NativeAotProjectWorkspace::Cleanup(std::string& outError)
{
	outError.clear();
	if(workspaceRoot_.empty()) {
		ResetWorkspacePaths();
		return true;
	}

#ifdef _WIN32
	if(!aliasPath_.empty()) {
		const DWORD attributes = GetFileAttributesW(aliasPath_.c_str());
		if(attributes != INVALID_FILE_ATTRIBUTES) {
			DWORD tagError = ERROR_SUCCESS;
			if((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U
				|| !IsMountPointJunction(aliasPath_, tagError)) {
				outError = "Refusing to clean the NativeAOT workspace because "
					"its project alias is not the owned mount-point junction";
				if(tagError != ERROR_SUCCESS) {
					outError += ": " + WindowsErrorMessage(tagError);
				}
				return false;
			}

			std::error_code equivalentError;
			if(!std::filesystem::equivalent(
					aliasPath_, sourceProjectRoot_, equivalentError)
				|| equivalentError) {
				outError = "Refusing to clean the NativeAOT workspace because "
					"its project junction no longer resolves to the exact source";
				if(equivalentError) {
					outError += ": " + equivalentError.message();
				}
				return false;
			}

			if(!RemoveDirectoryW(aliasPath_.c_str())) {
				outError = "Could not remove the verified NativeAOT project "
					"junction: " + WindowsErrorMessage(GetLastError());
				return false;
			}
		}
		else {
			const DWORD attributeError = GetLastError();
			if(attributeError != ERROR_FILE_NOT_FOUND
				&& attributeError != ERROR_PATH_NOT_FOUND) {
				outError = "Could not inspect the NativeAOT project junction: "
					+ WindowsErrorMessage(attributeError);
				return false;
			}
		}
	}
#endif

	// The project alias is gone before recursion begins, so remove_all cannot
	// descend into authored content even if a filesystem implementation follows
	// directory reparse points while walking.
	std::error_code cleanupError;
	std::filesystem::remove_all(workspaceRoot_, cleanupError);
	if(cleanupError) {
		outError = "Could not remove the private NativeAOT workspace "
			+ PathForMessage(workspaceRoot_) + ": " + cleanupError.message();
		return false;
	}

	ResetWorkspacePaths();
	return true;
}

const std::filesystem::path&
NativeAotProjectWorkspace::SourceProjectRoot() const noexcept
{
	return sourceProjectRoot_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::BuildProjectRoot() const noexcept
{
	return buildProjectRoot_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::WorkspaceRoot() const noexcept
{
	return workspaceRoot_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::IntermediateOutputDirectory() const noexcept
{
	return intermediateOutputDirectory_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::BinaryOutputDirectory() const noexcept
{
	return binaryOutputDirectory_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::PublishDirectory() const noexcept
{
	return publishDirectory_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::PackageCacheDirectory() const noexcept
{
	return packageCacheDirectory_;
}

const std::filesystem::path&
NativeAotProjectWorkspace::DotnetCliHomeDirectory() const noexcept
{
	return dotnetCliHomeDirectory_;
}

bool NativeAotProjectWorkspace::UsesShortAlias() const noexcept
{
	return !aliasPath_.empty();
}

void NativeAotProjectWorkspace::ResetWorkspacePaths() noexcept
{
	buildProjectRoot_ = sourceProjectRoot_;
	workspaceRoot_.clear();
	aliasPath_.clear();
	intermediateOutputDirectory_.clear();
	binaryOutputDirectory_.clear();
	publishDirectory_.clear();
	packageCacheDirectory_.clear();
	dotnetCliHomeDirectory_.clear();
}
