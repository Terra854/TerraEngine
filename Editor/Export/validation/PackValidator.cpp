// PackValidator.cpp — TEPAK archive validation implementation
//
// Implements the structural and integrity validation rules for TEPAK archives. It performs comprehensive checks on the directory hierarchy, path uniqueness, and data checksums to ensure that archives are fully compliant with the format specification.

#include "TEPAK/PackValidator.h"

#include "archive/ArchiveWorkflow.h"
#include "codec/CodecProvider.h"
#include "io/PathNormalizer.h"
#include "io/PathText.h"
#include "validation/ValidationReport.h"

#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace {

// ReadStoredBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
tepak::ByteBuffer ReadStoredBytes(const tepak::archive::OpenedPack& openedPack, const tepak::PackExtent& extent) {
	// Validation reads raw stored bytes exactly as the runtime reader would so
	// decompression and hashing are exercised against the real archive layout.
	std::ifstream stream(openedPack.packPath, std::ios::binary);
	if (!stream) {
		throw std::runtime_error("Failed to open the pack for validation.");
	}

	tepak::ByteBuffer storedBytes(extent.storedSize);
	stream.seekg(static_cast<std::streamoff>(extent.storedOffset), std::ios::beg);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extent.storedSize > 0U) {
		stream.read(reinterpret_cast<char*>(storedBytes.data()), static_cast<std::streamsize>(extent.storedSize));
	}
	return storedBytes;
}

} // namespace

namespace tepak::validation {

void AddFinding(ValidationReport& report,
	ValidationSeverity severity,
	std::string category,
	std::string location,
	std::string ruleIdentifier,
	std::string message,
	std::string entryPath) {
	// Validation keeps every finding instead of failing fast so operators can
	// fix an entire broken archive in one pass.
	report.findings.push_back({severity, std::move(category), std::move(location), std::move(ruleIdentifier), std::move(message), std::move(entryPath)});
	if (severity == ValidationSeverity::Error) {
		++report.errorCount;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (severity == ValidationSeverity::Warning) {
		++report.warningCount;
	}
}

} // namespace tepak::validation

namespace tepak {

// PackValidator::Validate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ValidationReport PackValidator::Validate(const std::filesystem::path& packPath, bool deepReadback) {
	ValidationReport report;
	report.packPath = packPath;
	report.integrityChecksPerformed = deepReadback;

	try {
		const auto openedPack = archive::OpenPackFile(packPath);
		report.checkedDirectoryCount = static_cast<std::uint32_t>(openedPack.directories.size());
		report.checkedEntryCount = static_cast<std::uint32_t>(openedPack.entries.size());
		report.checkedExtentCount = static_cast<std::uint32_t>(openedPack.extents.size());

		// First pass: validate the directory tree and basic path uniqueness rules
		// without touching payload bytes.
		std::unordered_set<std::string> seenDirectories;
		for (const auto& directory : openedPack.directories) {
			if (!seenDirectories.insert(directory.normalizedPath).second) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"hierarchy",
					directory.normalizedPath,
					"duplicate-directory",
					"The pack contains duplicate normalized directory paths.",
					directory.normalizedPath);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (directory.directoryId == openedPack.descriptor.rootDirectoryId) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (directory.parentDirectoryId != kInvalidDirectoryId) {
					validation::AddFinding(report,
						ValidationSeverity::Error,
						"hierarchy",
						directory.normalizedPath,
						"root-parent",
						"The root directory must not have a parent directory.",
						directory.normalizedPath);
				}
			} else if (directory.parentDirectoryId >= openedPack.directories.size()) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"hierarchy",
					directory.normalizedPath,
					"directory-parent",
					"A directory references an invalid parent directory.",
					directory.normalizedPath);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (directory.childDirectoryCount > 0U &&
				(static_cast<std::uint64_t>(directory.firstChildDirectoryIndex) + directory.childDirectoryCount) > openedPack.directories.size()) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"hierarchy",
					directory.normalizedPath,
					"directory-child-span",
					"A directory child span points outside the directory table.",
					directory.normalizedPath);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (directory.childFileCount > 0U &&
				(static_cast<std::uint64_t>(directory.firstChildFileIndex) + directory.childFileCount) > openedPack.entries.size()) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"hierarchy",
					directory.normalizedPath,
					"file-child-span",
					"A directory child span points outside the file table.",
					directory.normalizedPath);
			}
		}

		std::unordered_set<std::string> seenPaths;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : openedPack.entries) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!seenPaths.insert(entry.normalizedPath).second) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"metadata",
					entry.normalizedPath,
					"duplicate-path",
					"The pack contains duplicate normalized paths.",
					entry.normalizedPath);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (seenDirectories.contains(entry.normalizedPath)) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"hierarchy",
					entry.normalizedPath,
					"path-collision",
					"A file path collides with a directory path.",
					entry.normalizedPath);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (entry.parentDirectoryId >= openedPack.directories.size()) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"hierarchy",
					entry.normalizedPath,
					"entry-parent",
					"A file entry references an invalid parent directory.",
					entry.normalizedPath);
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& directory : openedPack.directories) {
			for (std::uint32_t index = 0U; index < directory.childDirectoryCount; ++index) {
				const auto& childDirectory = openedPack.directories.at(directory.firstChildDirectoryIndex + index);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (childDirectory.parentDirectoryId != directory.directoryId) {
					validation::AddFinding(report,
						ValidationSeverity::Error,
						"hierarchy",
						directory.normalizedPath,
						"child-directory-parent",
						"A child directory span contains an entry with the wrong parent directory.",
						childDirectory.normalizedPath);
				}
			}
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::uint32_t index = 0U; index < directory.childFileCount; ++index) {
				const auto& childEntry = openedPack.entries.at(directory.firstChildFileIndex + index);
				if (childEntry.parentDirectoryId != directory.directoryId) {
					validation::AddFinding(report,
						ValidationSeverity::Error,
						"hierarchy",
						directory.normalizedPath,
						"child-file-parent",
						"A child file span contains an entry with the wrong parent directory.",
						childEntry.normalizedPath);
				}
			}
		}

		// Validation first checks structural rules, then optionally performs full
		// integrity reads so operators can catch corruption before adoption.
		const codec::CodecProvider codecProvider;
		for (const auto& entry : openedPack.entries) {
			std::uint64_t accumulatedStoredSize = 0U;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::uint32_t index = 0U; index < entry.entryExtentCount; ++index) {
				const auto& extent = openedPack.extents.at(entry.firstExtentIndex + index);
				accumulatedStoredSize += extent.storedSize;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if ((extent.storedOffset + extent.storedSize) > openedPack.descriptor.archiveSize) {
					validation::AddFinding(report,
						ValidationSeverity::Error,
						"extent",
						entry.normalizedPath,
						"stored-range",
						"An extent points outside the pack payload region.",
						entry.normalizedPath);
					continue;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (deepReadback) {
					// Deep readback verifies both the stored compressed bytes and the
					// reconstructed logical bytes against recorded hashes.
					const auto storedBytes = ReadStoredBytes(openedPack, extent);
					const auto decodedBytes = codecProvider.Decompress(storedBytes,
						extent.codec,
						static_cast<std::size_t>(extent.uncompressedSize),
						openedPack.dictionaryBytes,
						&openedPack.preparedDictionary);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (codecProvider.HashBytes(decodedBytes) != extent.extentHash) {
						validation::AddFinding(report,
							ValidationSeverity::Error,
							"integrity",
							entry.normalizedPath,
							"extent-hash",
							"An extent failed the decoded hash check.",
							entry.normalizedPath);
					}
				}
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (accumulatedStoredSize != entry.storedSize) {
				validation::AddFinding(report,
					ValidationSeverity::Error,
					"metadata",
					entry.normalizedPath,
					"stored-size",
					"The entry stored size does not match the sum of its extents.",
					entry.normalizedPath);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (deepReadback) {
				const auto reconstructed = archive::ReadLogicalBytes(openedPack, entry.normalizedPath, 0U, entry.logicalSize, true);
				if (codecProvider.HashBytes(reconstructed) != entry.entryHash) {
					validation::AddFinding(report,
						ValidationSeverity::Error,
						"integrity",
						entry.normalizedPath,
						"entry-hash",
						"The reconstructed entry bytes do not match the recorded hash.",
						entry.normalizedPath);
				}
			}
		}

		report.validationStatus = (report.errorCount == 0U);
		return report;
	} catch (const std::exception& exception) {
		validation::AddFinding(report,
			ValidationSeverity::Error,
			"open",
			io::PathToUtf8String(packPath),
			"open-pack",
			exception.what(),
			{});
		report.validationStatus = false;
		return report;
	}
}

} // namespace tepak
