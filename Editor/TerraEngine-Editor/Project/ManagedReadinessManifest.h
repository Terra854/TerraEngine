// ManagedReadinessManifest.h — validated managed gameplay readiness cache
//
// Stores the exact inputs that produced a loadable Assembly-CSharp DLL. The
// manifest is cache metadata, never project authority: a missing, stale, or
// corrupt record simply causes the normal managed build path to run again.

#pragma once

#include <filesystem>
#include <string>

// ManagedReadinessInputs contains every value that can make a previously built
// gameplay assembly unsafe to reuse. All values are stable hexadecimal FNV-1a
// fingerprints used for cache validation, not security signatures.
struct ManagedReadinessInputs
{
	std::string sourceFingerprint;
	std::string workspaceFingerprint;
	std::string buildProfile;
	std::string bridgeFingerprint;
	std::string analyzerFingerprint;
	std::string runtimeAbiFingerprint;
};

enum class ManagedReadinessValidation
{
	Valid,
	Missing,
	Corrupt,
	Stale,
	OutputMissing,
	OutputHashMismatch
};

// ManagedReadinessManifest is an immutable description of one successfully
// copied, built, and loaded Assembly-CSharp output. A new record is atomically
// replaced only after the caller has completed every one of those stages.
struct ManagedReadinessManifest
{
	static constexpr int kCurrentVersion = 1;

	int version = kCurrentVersion;
	ManagedReadinessInputs inputs;
	std::string outputFingerprint;

	[[nodiscard]] static std::filesystem::path GetPath(const std::filesystem::path& outputRoot);
	[[nodiscard]] static std::string FingerprintFile(const std::filesystem::path& path);
	[[nodiscard]] static std::string FingerprintDirectory(const std::filesystem::path& root, const std::filesystem::path& excludedRoot = {});
	[[nodiscard]] static ManagedReadinessValidation Validate(
		const std::filesystem::path& manifestPath,
		const ManagedReadinessInputs& expectedInputs,
		const std::filesystem::path& outputAssembly,
		ManagedReadinessManifest* outManifest = nullptr,
		std::string* outError = nullptr);

	[[nodiscard]] bool PersistAtomically(const std::filesystem::path& manifestPath, std::string* outError = nullptr) const;
};
