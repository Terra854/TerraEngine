// ExportEvidence.h - deterministic files emitted by every automated export.

#pragma once

#include "ExportTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ExportContentInventoryEntry {
	std::string logicalPath;
	std::string assetType;
	std::string sourceKind;
	std::uint64_t byteSize = 0;
	std::string contentHash;
	bool required = true;
	bool placeholderDetected = false;
};

struct ExportEvidenceInputs {
	std::string resolvedManifestJson;
	std::vector<ExportContentInventoryEntry> inventory;
	std::vector<std::string> progressJsonLines;
	std::string packageValidationReport;
};

/// Scans the staged package input and produces a stable, lexically sorted
/// inventory. File reads happen independently, so callers may schedule this
/// function on a worker while the protected publication boundary remains idle.
bool BuildExportContentInventory(
	const std::filesystem::path& rootDirectory,
	std::vector<ExportContentInventoryEntry>& outEntries,
	std::string& outError);

void NormalizeExportContentInventory(std::vector<ExportContentInventoryEntry>& entries);
bool EquivalentExportContentInventories(
	std::vector<ExportContentInventoryEntry> left,
	std::vector<ExportContentInventoryEntry> right);

/// Removes only explicitly non-product invocation metadata and returns compact,
/// key-sorted JSON suitable for editor/headless parity comparison.
std::string NormalizeResolvedManifestForParity(const std::string& manifestJson);

std::string SerializeExportProgressRecord(const std::string& runId, const ExportProgress& progress);
std::string SerializeExportResultRecord(const ExportResult& result);

/// Writes the six-file evidence contract. Later-stage files may contain an
/// explanatory partial record after a failed run, but every file is still
/// created once a request has a resolved evidence directory.
bool WriteExportEvidenceBundle(
	const ExportRequest& request,
	const ExportResult& result,
	const ExportEvidenceInputs& inputs,
	std::string& outError);
