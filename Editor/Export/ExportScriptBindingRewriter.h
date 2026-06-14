// ExportScriptBindingRewriter.h - Helpers for preparing staged script bindings
// for the NativeAOT export runtime.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// ComputeExportScriptBindingId returns the deterministic FNV-1a identifier
// shared by editor staging, generated NativeAOT registries, and runtime lookup.
std::uint64_t ComputeExportScriptBindingId(const std::string& className);

// RewriteStagedScriptBindings rewrites only staged scene and prefab copies so
// authored editor assets keep class names while exports use numeric script IDs.
bool RewriteStagedScriptBindings(const std::filesystem::path& stagingDir, std::string& outError);
