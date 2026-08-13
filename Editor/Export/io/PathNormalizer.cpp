// PathNormalizer.cpp — TEPAK path normalization implementation
//
// Implements the detailed path processing logic, ensuring logical paths are stored in a stable, platform-independent format. It handles the discovery of packaging inventories and manages the selection of files for shared dictionary training.

#include "io/PathNormalizer.h"
#include "io/PathText.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace {

constexpr std::size_t kSampleBytesPerFile = 32U * 1024U;
constexpr std::size_t kMaxDictionarySampleBytes = 64U * 1024U * 1024U;

// IsSelected is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool IsSelected(const tepak::PackagingRequest& request, const std::filesystem::path& path) {
	if (request.entrySelectionRules.empty()) {
		return true;
	}

	// Selection rules are suffix matches so callers can cheaply target asset
	// classes or subtrees without having to supply a full glob engine.
	const auto genericPath = tepak::io::PathToUtf8String(path);
	return std::any_of(request.entrySelectionRules.begin(), request.entrySelectionRules.end(), [&](const std::string& rule) {
		return genericPath.ends_with(rule);
	});
}

// IsDictionarySampleEligible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool IsDictionarySampleEligible(const std::string& assetClass) {
	// Audio and video are usually already compressed, so sampling them tends to
	// waste training budget without improving a shared dictionary.
	return assetClass != "audio" && assetClass != "video";
}

// ReadDictionarySample is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
tepak::ByteBuffer ReadDictionarySample(const std::filesystem::path& path, std::size_t bytesToRead) {
	// Only the front slice of each file is sampled because TEPAK wants a cheap,
	// bounded training set rather than another full-file buffering pass.
	tepak::ByteBuffer sample(bytesToRead);
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		throw std::runtime_error("Failed to open staged file sample: " + tepak::io::PathToUtf8String(path));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (bytesToRead > 0U) {
		stream.read(reinterpret_cast<char*>(sample.data()), static_cast<std::streamsize>(bytesToRead));
		if (!stream) {
			throw std::runtime_error("Failed to read staged file sample: " + tepak::io::PathToUtf8String(path));
		}
	}
	return sample;
}

// ParentDirectoryPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ParentDirectoryPath(std::string_view normalizedPath) {
	const auto separator = normalizedPath.rfind('/');
	return separator == std::string_view::npos ? std::string{} : std::string(normalizedPath.substr(0U, separator));
}

// struct DirectoryAccumulator groups related state and operations so callers have one documented contract.
struct DirectoryAccumulator final {
	std::string normalizedPath;
	std::string parentPath;
	std::vector<std::string> childDirectories;
	std::vector<tepak::model::StagedFile> childFiles;
};

} // namespace

namespace tepak::io {

// NormalizeRelativePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string NormalizeRelativePath(const std::filesystem::path& inputRoot, const std::filesystem::path& sourcePath) {
	// TEPAK stores logical paths in a stable archive-wide form so different
	// source directory spellings do not create duplicate entries.
	const auto relativePath = std::filesystem::relative(sourcePath, inputRoot);
	if (relativePath.empty()) {
		throw std::runtime_error("Source path is not inside the declared input root.");
	}

	std::vector<std::string> segments;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& segment : relativePath) {
		const auto text = PathToUtf8String(segment);
		if (text == ".") {
			continue;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (text == "..") {
			throw std::runtime_error("Normalized paths may not traverse above the input root.");
		}
		segments.push_back(text);
	}

	std::string normalized;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < segments.size(); ++index) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (index > 0U) {
			normalized.push_back('/');
		}
		normalized += segments[index];
	}

	LowercaseAsciiInPlace(normalized);
	return normalized;
}

// NormalizeLookupPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string NormalizeLookupPath(std::string_view logicalPath) {
	std::string normalized;
	normalized.reserve(logicalPath.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const char value : logicalPath) {
		normalized.push_back(value == '\\' ? '/' : value);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (!normalized.empty() && normalized.front() == '/') {
		normalized.erase(normalized.begin());
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (normalized.starts_with("./")) {
		normalized.erase(0U, 2U);
	}
	LowercaseAsciiInPlace(normalized);
	return normalized;
}

// ExtractLeafName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ExtractLeafName(std::string_view normalizedPath) {
	if (normalizedPath.empty()) {
		return {};
	}

	const auto separator = normalizedPath.rfind('/');
	return separator == std::string_view::npos ? std::string(normalizedPath) : std::string(normalizedPath.substr(separator + 1U));
}

// EnumerateDirectoryPaths is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<std::string> EnumerateDirectoryPaths(std::string_view normalizedFilePath) {
	std::vector<std::string> directories;
	std::string current;
	// "meshes/props/cube.obj" contributes "meshes" and "meshes/props" so the
	// archive can recreate implicit directories from file paths alone.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const char value : normalizedFilePath) {
		if (value == '/') {
			if (!current.empty()) {
				directories.push_back(current);
			}
		}
		current.push_back(value);
	}
	return directories;
}

// DetectAssetClass is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string DetectAssetClass(const std::filesystem::path& sourcePath) {
	std::string extension = PathToUtf8String(sourcePath.extension());
	LowercaseAsciiInPlace(extension);

	// Animation classes are persisted as append-only TEPAK codes. Classify the
	// cooked extension before the broader data/other buckets so pack metadata is
	// useful to runtime discovery without opening every payload first.
	if (extension == ".teskel") {
		return "skeleton";
	}
	if (extension == ".teanim") {
		return "animation-clip";
	}
	if (extension == ".teanimctrl") {
		return "animation-controller";
	}
	if (extension == ".teanimmask") {
		return "animation-mask";
	}
	if (extension == ".teretarget") {
		return "retarget-profile";
	}
	if (extension == ".temotiondb") {
		return "motion-database";
	}
	if (extension == ".teragdoll") {
		return "ragdoll-profile";
	}
	// Cooked audio is the runtime authority. Classify the TEAF envelope itself,
	// not only source media, so ContentMount can fail closed on wrong metadata.
	if (extension == ".teaudio") {
		return "audio";
	}

	// Asset classes are coarse by design. They are primarily used for summaries
	// and future policy hooks, not as a strict runtime type system.
	if (extension == ".vert" || extension == ".frag" || extension == ".comp" || extension == ".hlsl" || extension == ".glsl") {
		return "shader";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".dds" || extension == ".ktx" || extension == ".hdr" || extension == ".exr") {
		return "texture";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".obj" || extension == ".gltf" || extension == ".glb" || extension == ".mesh" || extension == ".temf") {
		return "mesh";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".wav" || extension == ".flac" || extension == ".ogg" || extension == ".mp3" || extension == ".m4a" || extension == ".wma") {
		return "audio";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".y4m" || extension == ".avi" || extension == ".mov" || extension == ".mp4" || extension == ".mkv" || extension == ".webm" || extension == ".flv") {
		return "video";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".json" || extension == ".txt" || extension == ".xml" || extension == ".csv" || extension == ".bin") {
		return "data";
	}
	return "other";
}

// DiscoverPackagingInventory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
model::PackagingInventory DiscoverPackagingInventory(const PackagingRequest& request) {
	// The discovery pass keeps only file metadata and bounded dictionary samples
	// in memory so archive size no longer dictates export memory usage.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(request.inputRoot)) {
		throw std::runtime_error("The input root does not exist.");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::is_directory(request.inputRoot)) {
		throw std::runtime_error("The input root must be a directory.");
	}

	model::PackagingInventory inventory;
	std::vector<model::StagedFile> discoveredFiles;
	std::unordered_set<std::string> filePaths;
	std::unordered_set<std::string> directoryPaths;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::recursive_directory_iterator(request.inputRoot)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsSelected(request, entry.path())) {
			continue;
		}

		model::StagedFile stagedFile;
		stagedFile.sourcePath = entry.path();
		stagedFile.normalizedPath = NormalizeRelativePath(request.inputRoot, entry.path());
		stagedFile.name = ExtractLeafName(stagedFile.normalizedPath);
		stagedFile.assetClass = DetectAssetClass(entry.path());
		stagedFile.logicalSize = entry.file_size();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!filePaths.insert(stagedFile.normalizedPath).second) {
			throw std::runtime_error("Duplicate normalized path detected: " + stagedFile.normalizedPath);
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& directoryPath : EnumerateDirectoryPaths(stagedFile.normalizedPath)) {
			directoryPaths.insert(directoryPath);
		}

		inventory.totalInputBytes += stagedFile.logicalSize;
		discoveredFiles.push_back(std::move(stagedFile));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (discoveredFiles.empty()) {
		throw std::runtime_error("The staged input set does not contain any eligible files.");
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& directoryPath : directoryPaths) {
		if (filePaths.contains(directoryPath)) {
			throw std::runtime_error("A normalized file path collides with an implied directory path: " + directoryPath);
		}
	}

	std::map<std::string, DirectoryAccumulator> directories;
	directories.emplace("", DirectoryAccumulator{});
	directories[""].normalizedPath = "";

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& directoryPath : directoryPaths) {
		auto& accumulator = directories[directoryPath];
		accumulator.normalizedPath = directoryPath;
		accumulator.parentPath = ParentDirectoryPath(directoryPath);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& stagedFile : discoveredFiles) {
		const auto parentPath = ParentDirectoryPath(stagedFile.normalizedPath);
		directories[parentPath].childFiles.push_back(std::move(stagedFile));
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [path, accumulator] : directories) {
		if (path.empty()) {
			continue;
		}
		auto& parent = directories[accumulator.parentPath];
		parent.childDirectories.push_back(path);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [path, accumulator] : directories) {
		std::sort(accumulator.childDirectories.begin(), accumulator.childDirectories.end(), [](const std::string& left, const std::string& right) {
			const auto leftName = ExtractLeafName(left);
			const auto rightName = ExtractLeafName(right);
			return leftName == rightName ? left < right : leftName < rightName;
		});
		accumulator.childDirectories.erase(std::unique(accumulator.childDirectories.begin(), accumulator.childDirectories.end()), accumulator.childDirectories.end());
		std::sort(accumulator.childFiles.begin(), accumulator.childFiles.end(), [](const model::StagedFile& left, const model::StagedFile& right) {
			return left.name == right.name ? left.normalizedPath < right.normalizedPath : left.name < right.name;
		});
	}

	std::vector<std::string> directoryOrder;
	directoryOrder.push_back("");
	std::unordered_map<std::string, std::uint32_t> firstChildDirectoryIndex;
	std::unordered_map<std::string, std::uint32_t> childDirectoryCount;
	// Breadth-first directory order guarantees that each directory's children
	// occupy one contiguous span later in the serialized directory table.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0U; index < directoryOrder.size(); ++index) {
		const auto& directoryPath = directoryOrder[index];
		const auto& children = directories.at(directoryPath).childDirectories;
		firstChildDirectoryIndex[directoryPath] = static_cast<std::uint32_t>(directoryOrder.size());
		childDirectoryCount[directoryPath] = static_cast<std::uint32_t>(children.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& childPath : children) {
			directoryOrder.push_back(childPath);
		}
	}

	std::unordered_map<std::string, std::uint32_t> directoryIds;
	inventory.directories.reserve(directoryOrder.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t directoryId = 0U; directoryId < directoryOrder.size(); ++directoryId) {
		directoryIds.emplace(directoryOrder[directoryId], directoryId);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t directoryId = 0U; directoryId < directoryOrder.size(); ++directoryId) {
		const auto& directoryPath = directoryOrder[directoryId];
		const auto& accumulator = directories.at(directoryPath);
		DirectoryNode node;
		node.directoryId = directoryId;
		node.parentDirectoryId = directoryPath.empty() ? kInvalidDirectoryId : directoryIds.at(accumulator.parentPath);
		node.normalizedPath = directoryPath;
		node.firstChildDirectoryIndex = firstChildDirectoryIndex.at(directoryPath);
		node.childDirectoryCount = childDirectoryCount.at(directoryPath);
		inventory.directories.push_back(std::move(node));
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& node : inventory.directories) {
		const auto& files = directories.at(node.normalizedPath).childFiles;
		node.firstChildFileIndex = static_cast<std::uint32_t>(inventory.stagedFiles.size());
		node.childFileCount = static_cast<std::uint32_t>(files.size());
		// Files are appended directory by directory so each directory node can
		// point at a contiguous file span in the final file table.
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& file : files) {
			auto orderedFile = file;
			orderedFile.parentDirectoryId = node.directoryId;
			inventory.stagedFiles.push_back(std::move(orderedFile));
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (request.enableCompression && request.enableDictionaryTraining) {
		// Raw-only packs never consult a dictionary, so sampling would only add
		// avoidable resident memory and planning work before the first byte is written.
		std::size_t sampledBytes = 0U;
		for (const auto& stagedFile : inventory.stagedFiles) {
			if (!IsDictionarySampleEligible(stagedFile.assetClass) || stagedFile.logicalSize == 0U || sampledBytes >= kMaxDictionarySampleBytes) {
				continue;
			}

			const auto sampleSize = static_cast<std::size_t>(std::min<std::uint64_t>(stagedFile.logicalSize, kSampleBytesPerFile));
			const auto cappedSampleSize = std::min<std::size_t>(sampleSize, kMaxDictionarySampleBytes - sampledBytes);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (cappedSampleSize == 0U) {
				continue;
			}

			// Sampling is capped globally so dictionary selection stays predictable
			// even when the source tree is extremely large.
			inventory.dictionarySamples.push_back(ReadDictionarySample(stagedFile.sourcePath, cappedSampleSize));
			sampledBytes += cappedSampleSize;
		}
	}

	return inventory;
}

} // namespace tepak::io
