#pragma once

#include <filesystem>
#include <string>

bool CompileShader(const std::filesystem::path& input, const std::filesystem::path& output, const std::string& stage, const std::string& targetProfile, const std::string& entryPoint, const std::filesystem::path& dxcPath);
