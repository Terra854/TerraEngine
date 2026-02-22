#pragma once

#include <filesystem>

// Compiles an input mesh into a meshlet-only TEMF artifact.
// Input: triangulated-compatible source mesh path.
// Output: .temf file containing packed vertices, 32-bit primitive indices, and meshlet tables.
// Returns false and emits diagnostics to stderr when parsing, meshlet generation, or file write fails.
bool CompileMesh(const std::filesystem::path& input, const std::filesystem::path& output);
