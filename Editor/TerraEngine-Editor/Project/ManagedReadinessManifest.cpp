// ManagedReadinessManifest.cpp — durable managed gameplay readiness cache

#include "ManagedReadinessManifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
	constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
	constexpr std::uint64_t kFnvPrime = 1099511628211ull;

	void HashBytes(std::uint64_t& hash, const char* bytes, std::size_t count)
	{
		for (std::size_t index = 0; index < count; ++index)
		{
			hash ^= static_cast<std::uint8_t>(bytes[index]);
			hash *= kFnvPrime;
		}
	}

	void HashText(std::uint64_t& hash, const std::string& text)
	{
		HashBytes(hash, text.data(), text.size());
		const char separator = '\0';
		HashBytes(hash, &separator, 1u);
	}

	std::string ToHex(std::uint64_t hash)
	{
		std::ostringstream output;
		output << std::hex << std::setfill('0') << std::setw(16) << hash;
		return output.str();
	}

	nlohmann::json ToJson(const ManagedReadinessManifest& manifest)
	{
		return {
			{"version", manifest.version},
			{"sourceFingerprint", manifest.inputs.sourceFingerprint},
			{"workspaceFingerprint", manifest.inputs.workspaceFingerprint},
			{"buildProfile", manifest.inputs.buildProfile},
			{"bridgeFingerprint", manifest.inputs.bridgeFingerprint},
			{"analyzerFingerprint", manifest.inputs.analyzerFingerprint},
			{"runtimeAbiFingerprint", manifest.inputs.runtimeAbiFingerprint},
			{"outputFingerprint", manifest.outputFingerprint}
		};
	}

	bool FromJson(const nlohmann::json& document, ManagedReadinessManifest& outManifest)
	{
		if (!document.is_object()
			|| !document.contains("version") || !document.at("version").is_number_integer()
			|| !document.contains("sourceFingerprint") || !document.at("sourceFingerprint").is_string()
			|| !document.contains("workspaceFingerprint") || !document.at("workspaceFingerprint").is_string()
			|| !document.contains("buildProfile") || !document.at("buildProfile").is_string()
			|| !document.contains("bridgeFingerprint") || !document.at("bridgeFingerprint").is_string()
			|| !document.contains("analyzerFingerprint") || !document.at("analyzerFingerprint").is_string()
			|| !document.contains("runtimeAbiFingerprint") || !document.at("runtimeAbiFingerprint").is_string()
			|| !document.contains("outputFingerprint") || !document.at("outputFingerprint").is_string())
		{
			return false;
		}

		outManifest.version = document.at("version").get<int>();
		outManifest.inputs.sourceFingerprint = document.at("sourceFingerprint").get<std::string>();
		outManifest.inputs.workspaceFingerprint = document.at("workspaceFingerprint").get<std::string>();
		outManifest.inputs.buildProfile = document.at("buildProfile").get<std::string>();
		outManifest.inputs.bridgeFingerprint = document.at("bridgeFingerprint").get<std::string>();
		outManifest.inputs.analyzerFingerprint = document.at("analyzerFingerprint").get<std::string>();
		outManifest.inputs.runtimeAbiFingerprint = document.at("runtimeAbiFingerprint").get<std::string>();
		outManifest.outputFingerprint = document.at("outputFingerprint").get<std::string>();
		return true;
	}

	bool InputsEqual(const ManagedReadinessInputs& left, const ManagedReadinessInputs& right)
	{
		return left.sourceFingerprint == right.sourceFingerprint
			&& left.workspaceFingerprint == right.workspaceFingerprint
			&& left.buildProfile == right.buildProfile
			&& left.bridgeFingerprint == right.bridgeFingerprint
			&& left.analyzerFingerprint == right.analyzerFingerprint
			&& left.runtimeAbiFingerprint == right.runtimeAbiFingerprint;
	}

	void SetError(std::string* outError, const std::string& message)
	{
		if (outError != nullptr)
		{
			*outError = message;
		}
	}
}

std::filesystem::path ManagedReadinessManifest::GetPath(const std::filesystem::path& outputRoot)
{
	return outputRoot / "ManagedReadiness.manifest.json";
}

std::string ManagedReadinessManifest::FingerprintFile(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		return {};
	}

	std::uint64_t hash = kFnvOffsetBasis;
	std::array<char, 4096> buffer{};
	while (input)
	{
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize count = input.gcount();
		if (count > 0)
		{
			HashBytes(hash, buffer.data(), static_cast<std::size_t>(count));
		}
	}
	return ToHex(hash);
}

std::string ManagedReadinessManifest::FingerprintDirectory(const std::filesystem::path& root, const std::filesystem::path& excludedRoot)
{
	std::error_code error;
	if (root.empty() || !std::filesystem::is_directory(root, error) || error)
	{
		return {};
	}

	std::vector<std::filesystem::path> files;
	for (std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error), end; iterator != end && !error; iterator.increment(error))
	{
		if (!excludedRoot.empty() && iterator->path().lexically_normal().string().rfind(excludedRoot.lexically_normal().string(), 0u) == 0u)
		{
			iterator.disable_recursion_pending();
			continue;
		}
		if (iterator->is_regular_file(error) && !error)
		{
			files.push_back(iterator->path());
		}
	}
	if (error)
	{
		return {};
	}

	std::sort(files.begin(), files.end(), [&](const std::filesystem::path& left, const std::filesystem::path& right) {
		return std::filesystem::relative(left, root).generic_string() < std::filesystem::relative(right, root).generic_string();
	});

	std::uint64_t hash = kFnvOffsetBasis;
	for (const std::filesystem::path& file : files)
	{
		std::error_code relativeError;
		const std::filesystem::path relativePath = std::filesystem::relative(file, root, relativeError);
		const std::string fileHash = FingerprintFile(file);
		if (relativeError || fileHash.empty())
		{
			return {};
		}
		HashText(hash, relativePath.generic_string());
		HashText(hash, fileHash);
	}
	return ToHex(hash);
}

ManagedReadinessValidation ManagedReadinessManifest::Validate(
	const std::filesystem::path& manifestPath,
	const ManagedReadinessInputs& expectedInputs,
	const std::filesystem::path& outputAssembly,
	ManagedReadinessManifest* outManifest,
	std::string* outError)
{
	std::ifstream input(manifestPath, std::ios::binary);
	if (!input)
	{
		SetError(outError, "The managed readiness manifest is unavailable.");
		return ManagedReadinessValidation::Missing;
	}

	try
	{
		nlohmann::json document;
		input >> document;
		ManagedReadinessManifest manifest;
		if (!FromJson(document, manifest) || manifest.version != kCurrentVersion)
		{
			SetError(outError, "The managed readiness manifest is corrupt or uses an unsupported version.");
			return ManagedReadinessValidation::Corrupt;
		}
		if (!InputsEqual(manifest.inputs, expectedInputs))
		{
			SetError(outError, "The managed readiness manifest is stale for the current sources, workspace, tools, or runtime ABI.");
			return ManagedReadinessValidation::Stale;
		}
		if (outputAssembly.empty() || !std::filesystem::is_regular_file(outputAssembly))
		{
			SetError(outError, "The managed readiness output DLL is unavailable.");
			return ManagedReadinessValidation::OutputMissing;
		}
		if (manifest.outputFingerprint.empty() || manifest.outputFingerprint != FingerprintFile(outputAssembly))
		{
			SetError(outError, "The managed readiness output DLL hash no longer matches its manifest.");
			return ManagedReadinessValidation::OutputHashMismatch;
		}

		if (outManifest != nullptr)
		{
			*outManifest = std::move(manifest);
		}
		SetError(outError, {});
		return ManagedReadinessValidation::Valid;
	}
	catch (const std::exception& exception)
	{
		SetError(outError, std::string("Unable to read the managed readiness manifest: ") + exception.what());
		return ManagedReadinessValidation::Corrupt;
	}
}

bool ManagedReadinessManifest::PersistAtomically(const std::filesystem::path& manifestPath, std::string* outError) const
{
	if (version != kCurrentVersion || outputFingerprint.empty())
	{
		SetError(outError, "A managed readiness manifest must contain the current version and an output fingerprint.");
		return false;
	}

	std::error_code directoryError;
	std::filesystem::create_directories(manifestPath.parent_path(), directoryError);
	if (directoryError)
	{
		SetError(outError, "Failed to create the managed readiness directory: " + directoryError.message());
		return false;
	}

	const std::filesystem::path temporaryPath = manifestPath.string() + ".tmp";
	{
		std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			SetError(outError, "Failed to create the managed readiness manifest temporary file.");
			return false;
		}
		output << ToJson(*this).dump(2) << '\n';
		output.flush();
		if (!output)
		{
			std::filesystem::remove(temporaryPath);
			SetError(outError, "Failed to write the managed readiness manifest temporary file.");
			return false;
		}
	}

	std::error_code renameError;
#if defined(_WIN32)
	// MoveFileEx replaces the previous cache record in one filesystem operation.
	// Unlike remove-then-rename, a crash cannot leave a missing/torn manifest.
	if (!MoveFileExW(temporaryPath.c_str(), manifestPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		renameError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	}
#else
	std::filesystem::rename(temporaryPath, manifestPath, renameError);
#endif
	if (renameError)
	{
		std::filesystem::remove(temporaryPath);
		SetError(outError, "Failed to publish the managed readiness manifest: " + renameError.message());
		return false;
	}

	SetError(outError, {});
	return true;
}
