#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Normalized compile report consumed by CLI output and validation scripts.
struct MeshCompileReport
{
	std::filesystem::path assetPath;
	std::uint16_t formatVersion = 0;
	std::uint64_t sourceBytes = 0;
	std::uint64_t outputBytes = 0;
	std::uint64_t baselineBytes = 0;
	double compressionRatio = 0.0;
	bool baselinePass = true;
	bool deterministicReady = true;
	bool runtimeValidationPass = true;
	bool usedU16RemapEncoding = false;
	std::uint32_t meshletGlobalVertexCount = 0;
	std::string sourceHashHex;
	std::string status = "compile_fail";
	std::string failureReason;
};

// Compiles an input mesh into a meshlet-only TEMF artifact.
// Input: triangulated-compatible source mesh path.
// Output: .temf file containing packed shared vertices, uint8 primitive indices, remap data, and meshlet tables.
// Returns false and emits diagnostics to stderr when parsing, meshlet generation, or file write fails.
bool CompileMesh(
	const std::filesystem::path& input,
	const std::filesystem::path& output,
	bool includeMeshletBounds = true,
	MeshCompileReport* outReport = nullptr,
	float smoothingAngle = 60.0f,
	float weldThreshold = 1e-6f);
