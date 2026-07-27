// NativeAotProjectWorkspace.h - private short-lived .NET build workspaces.
//
// Windows child tools used by MSBuild and NativeAOT can still reject a long
// working path even when TerraEngine itself is long-path aware. This helper
// exposes the authored project through a short, verified junction and keeps
// every generated file in a unique temporary directory owned by one operation.

#pragma once

#include <filesystem>
#include <string>

class NativeAotProjectWorkspace final {
public:
	explicit NativeAotProjectWorkspace(std::filesystem::path sourceProjectRoot);
	~NativeAotProjectWorkspace();

	NativeAotProjectWorkspace(const NativeAotProjectWorkspace&) = delete;
	NativeAotProjectWorkspace& operator=(const NativeAotProjectWorkspace&) = delete;
	NativeAotProjectWorkspace(NativeAotProjectWorkspace&&) = delete;
	NativeAotProjectWorkspace& operator=(NativeAotProjectWorkspace&&) = delete;

	// Creates one private workspace. Windows always receives a short `p`
	// junction, while other hosts keep the original project path and still use
	// private intermediate/output directories.
	[[nodiscard]] bool Prepare(std::string& outError);

	// Removes the verified project junction before recursively deleting the
	// private workspace. Callers should report a false result; the destructor is
	// only a best-effort fallback for early-return and exception paths.
	[[nodiscard]] bool Cleanup(std::string& outError);

	[[nodiscard]] const std::filesystem::path& SourceProjectRoot() const noexcept;
	[[nodiscard]] const std::filesystem::path& BuildProjectRoot() const noexcept;
	[[nodiscard]] const std::filesystem::path& WorkspaceRoot() const noexcept;
	[[nodiscard]] const std::filesystem::path& IntermediateOutputDirectory() const noexcept;
	[[nodiscard]] const std::filesystem::path& BinaryOutputDirectory() const noexcept;
	[[nodiscard]] const std::filesystem::path& PublishDirectory() const noexcept;
	[[nodiscard]] const std::filesystem::path& PackageCacheDirectory() const noexcept;
	[[nodiscard]] const std::filesystem::path& DotnetCliHomeDirectory() const noexcept;
	[[nodiscard]] bool UsesShortAlias() const noexcept;

private:
	void ResetWorkspacePaths() noexcept;

	std::filesystem::path sourceProjectRoot_;
	std::filesystem::path buildProjectRoot_;
	std::filesystem::path workspaceRoot_;
	std::filesystem::path aliasPath_;
	std::filesystem::path intermediateOutputDirectory_;
	std::filesystem::path binaryOutputDirectory_;
	std::filesystem::path publishDirectory_;
	std::filesystem::path packageCacheDirectory_;
	std::filesystem::path dotnetCliHomeDirectory_;
};
