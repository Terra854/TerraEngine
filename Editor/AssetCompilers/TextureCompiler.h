// TextureCompiler.h — Texture compilation pipeline interface
//
// Defines the interface for the offline texture compilation system, supporting various source image formats. It specifies the configuration options for texture semantical mapping, quality levels, and mipmap generation.

#pragma once

#include <filesystem>
#include <string>
#include <cstdint>
#include "Engine/File Formats/TerraEngineTextureFile.h"

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

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic, std::uint32_t quality = 1, bool generateMipmaps = true, TextureFilterType filter = TextureFilterType::Lanczos);
