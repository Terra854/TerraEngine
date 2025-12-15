#pragma once

#include <filesystem>
#include "Engine/File Formats/TerraEngineTextureFile.h"

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic);
