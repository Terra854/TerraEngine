#pragma once

#include <filesystem>
#include <string>
#include <ktx.h>
#include "Engine/File Formats/TerraEngineTextureFile.h"

// Returns true when the file extension maps to a texture source format the
// offline texture compiler can load directly.
bool IsSupportedTextureSourceExtension(const std::filesystem::path& filePath);

// Builds a human-readable extension list for help text and diagnostics.
std::string GetSupportedTextureSourceExtensionsText();

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic, std::uint32_t quality = 1);
