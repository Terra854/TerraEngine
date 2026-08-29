// ExportRuntimeLayout.cpp - executable-relative installed tree discovery.

#include "ExportRuntimeLayout.h"

#include <system_error>

namespace {
	std::string PathToUtf8(const std::filesystem::path& path) {
		const std::u8string encoded = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	std::filesystem::path CanonicalExistingPath(
		const std::filesystem::path& path,
		std::string& outError)
	{
		std::error_code errorCode;
		const std::filesystem::path canonicalPath = std::filesystem::canonical(path, errorCode);
		if(errorCode) {
			outError = "Could not resolve installed export executable '" + PathToUtf8(path) + "': " + errorCode.message();
			return {};
		}
		return canonicalPath;
	}

	bool RequireDirectory(const std::filesystem::path& path, const char* label, std::string& outError) {
		std::error_code errorCode;
		if(!std::filesystem::is_directory(path, errorCode)) {
			outError = std::string("Required installed ") + label + " directory was not found: " + PathToUtf8(path);
			return false;
		}
		return true;
	}

	bool RequireFile(const std::filesystem::path& path, const char* label, std::string& outError) {
		std::error_code errorCode;
		if(!std::filesystem::is_regular_file(path, errorCode)) {
			outError = std::string("Required installed ") + label + " file was not found: " + PathToUtf8(path);
			return false;
		}
		return true;
	}
}

bool DiscoverExportRuntimeLayout(
	const std::filesystem::path& executablePath,
	ExportRuntimeLayout& outLayout,
	std::string& outError)
{
	outError.clear();
	const std::filesystem::path canonicalExecutable = CanonicalExistingPath(executablePath, outError);
	if(canonicalExecutable.empty()) {
		return false;
	}

	outLayout = {};
	outLayout.executablePath = canonicalExecutable;
	const std::filesystem::path executableDirectory = canonicalExecutable.parent_path();
	if(executableDirectory.filename() == "Editor") {
		outLayout.editorDirectory = executableDirectory;
		outLayout.installRoot = executableDirectory.parent_path();
		outLayout.exportRuntimeDirectory = outLayout.editorDirectory / "ExportRuntime";
	} else {
		// CMake keeps developer executables at <build-dir> while the required
		// install target materializes the self-contained tree below Build/. The
		// editor therefore resolves the same installed payload as the CLI without
		// depending on its working directory or forcing a different dev layout.
		outLayout.installRoot = executableDirectory / "Build";
		outLayout.editorDirectory = outLayout.installRoot / "Editor";
		outLayout.exportRuntimeDirectory = outLayout.editorDirectory / "ExportRuntime";
	}
	// Only the payload copied into a built game comes from ExportRuntime.
	outLayout.engineDirectory = outLayout.exportRuntimeDirectory;
	outLayout.launcherDirectory = outLayout.exportRuntimeDirectory;
	outLayout.shaderDirectory = outLayout.editorDirectory / "Shaders";
	outLayout.fontDirectory = outLayout.editorDirectory / "Fonts";
	outLayout.managedDirectory = outLayout.editorDirectory / "Managed";
	outLayout.dotnetSdkDirectory = outLayout.editorDirectory / "DotNet";
#ifdef _WIN32
	outLayout.dotnetExecutable = outLayout.dotnetSdkDirectory / "dotnet.exe";
#else
	outLayout.dotnetExecutable = outLayout.dotnetSdkDirectory / "dotnet";
#endif
	outLayout.nativeAotToolchainDirectory = outLayout.editorDirectory / "NativeAotNuGet";
	outLayout.nativeAotNugetConfig = outLayout.nativeAotToolchainDirectory / "NuGet.Config";
	outLayout.nativeAotNugetFeed = outLayout.nativeAotToolchainDirectory / "feed";
	outLayout.nativeAotGlobalJson = outLayout.nativeAotToolchainDirectory / "global.json";
#ifdef _WIN32
	outLayout.modelPackageCompiler = outLayout.editorDirectory / "TerraEngineModelPackageCompiler.exe";
#else
	outLayout.modelPackageCompiler = outLayout.editorDirectory / "TerraEngineModelPackageCompiler";
#endif

	return true;
}

bool ValidateExportRuntimeLayout(
	const ExportRuntimeLayout& layout,
	ExportPlatform platform,
	std::string& outError)
{
	outError.clear();
	if(!RequireDirectory(layout.editorDirectory, "Editor", outError)
		|| !RequireDirectory(layout.engineDirectory, "Engine", outError)
		|| !RequireDirectory(layout.launcherDirectory, "Launcher", outError)
		|| !RequireDirectory(layout.shaderDirectory, "shader", outError)
		|| !RequireDirectory(layout.fontDirectory, "font", outError)
		|| !RequireDirectory(layout.managedDirectory, "managed reference", outError)
		|| !RequireDirectory(layout.dotnetSdkDirectory, ".NET SDK", outError)
		|| !RequireFile(layout.dotnetExecutable, ".NET SDK executable", outError)
		|| !RequireDirectory(layout.nativeAotToolchainDirectory, "NativeAOT toolchain", outError)
		|| !RequireDirectory(layout.nativeAotNugetFeed, "NativeAOT offline feed", outError)
		|| !RequireFile(layout.nativeAotNugetConfig, "NativeAOT NuGet.Config", outError)
		|| !RequireFile(layout.nativeAotGlobalJson, "NativeAOT global.json", outError)
		|| !RequireFile(layout.modelPackageCompiler, "model package compiler", outError)) {
		return false;
	}

	const std::filesystem::path launcher = layout.launcherDirectory /
		GetExportLauncherFileName("TerraEngine-Launcher", platform);
	if(!RequireFile(launcher, "launcher", outError)) {
		return false;
	}

	const std::filesystem::path engineLibrary = layout.launcherDirectory /
		GetExportEngineLibraryName(platform);
	if(!RequireFile(engineLibrary, "engine runtime", outError)) {
		return false;
	}

	const std::filesystem::path glfwLibrary = layout.launcherDirectory /
		GetExportGlfwLibraryName(platform);
	return RequireFile(glfwLibrary, "GLFW runtime", outError);
}
