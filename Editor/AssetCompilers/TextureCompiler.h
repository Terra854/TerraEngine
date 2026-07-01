// TextureCompiler.h — Texture compilation pipeline interface
//
// Defines the interface for the offline texture compilation system, supporting various source image formats. It specifies the configuration options for texture semantical mapping, quality levels, and mipmap generation.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include "Engine/File Formats/TerraEngineTextureFile.h"

using TextureCompileProgressCallback = std::function<void(float progress, std::string_view stage, std::string_view message)>;

// Returns true when the file extension maps to a texture source format the
// offline texture compiler can load directly.
bool IsSupportedTextureSourceExtension(const std::filesystem::path& filePath);

// Builds a human-readable extension list for help text and diagnostics.
std::string GetSupportedTextureSourceExtensionsText();

enum class TextureFilterType {
	Box,
	Triangle,
	Lanczos,
	Kaiser
};

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic, std::uint32_t quality = 1, bool generateMipmaps = true, TextureFilterType filter = TextureFilterType::Lanczos, TextureCompileProgressCallback progressCallback = {});

// Compiles image bytes extracted from a package. The source extension tells the
// existing stb-backed loader how to decode the staged bytes.
bool CompileTextureFromBytes(
	const std::vector<std::uint8_t>& bytes,
	std::string sourceExtension,
	const std::filesystem::path& output,
	TextureFileSemantic semantic,
	std::uint32_t quality = 1,
	bool generateMipmaps = true,
	TextureFilterType filter = TextureFilterType::Lanczos,
	TextureCompileProgressCallback progressCallback = {});

// Exposes the texture compiler's worker-count policy to focused tests so MCP
// package imports cannot accidentally return to all-core compression bursts.
std::uint32_t ResolveTextureCompilerWorkerCountForTests(std::uint32_t hardwareThreads, const char* overrideText);
