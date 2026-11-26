// ShaderCompiler.h — Shader compilation pipeline interface
//
// Provides the interface for compiling HLSL source files into the engine's internal TSPF shader format. It defines the main entry point for the offline shader compiler tool.

#pragma once

#include <filesystem>
#include <string>

// Only explicitly opted-in fullscreen fragment assets use the separate binding
// and parameter contract. Ordinary imports retain the surface/geometry policy.
// SourceSet compiles editable engine templates for diagnostics. Runtime shader
// loading still enforces the authored geometry and material ABI.
enum class ShaderUse { Surface, PostProcess, SourceSet, CompatibleSurface };

bool CompileShader(const std::filesystem::path& input, const std::filesystem::path& output, const std::string& stage, const std::string& targetProfile, const std::string& entryPoint, const std::filesystem::path& dxcPath, ShaderUse use = ShaderUse::Surface);
