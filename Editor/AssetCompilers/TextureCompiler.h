#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include "Engine/File Formats/TerraEngineTextureFile.h"

// Returns true when the file extension maps to a texture source format the
// offline texture compiler can load directly.
bool IsSupportedTextureSourceExtension(const std::filesystem::path& filePath);

// Builds a human-readable extension list for help text and diagnostics.
std::string GetSupportedTextureSourceExtensionsText();

// The compiled artifact remains .tetf even though the payload has moved to
// KTX2. Keeping this centralized prevents the CLI and editor from drifting.
inline constexpr std::string_view kCompiledTextureExtension = ".tetf";

// Describe the current compiled texture contract in user-facing messages.
std::string GetCompiledTextureOutputDescription();

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic);
