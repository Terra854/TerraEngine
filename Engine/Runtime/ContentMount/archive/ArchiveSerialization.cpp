#include "archive/ArchiveWorkflow.h"

#include <unordered_map>

namespace tepak::archive {

std::uint32_t EncodeAssetClass(std::string_view assetClass) {
	// These small stable ids keep entry records compact while still preserving
	// enough type information for tools to show useful summaries.
	static const std::unordered_map<std::string_view, std::uint32_t> kCodes = {
		{"mesh", 1U},
		{"texture", 2U},
		{"shader", 3U},
		{"audio", 4U},
		{"video", 5U},
		{"data", 6U},
		{"other", 7U},
	};

	const auto found = kCodes.find(assetClass);
	return found == kCodes.end() ? 7U : found->second;
}

std::string DecodeAssetClass(std::uint32_t assetClassCode) {
	// Unknown codes intentionally degrade to "other" so older tools can still
	// inspect newer archives without crashing on an enum mismatch.
	switch (assetClassCode) {
	case 1U:
		return "mesh";
	case 2U:
		return "texture";
	case 3U:
		return "shader";
	case 4U:
		return "audio";
	case 5U:
		return "video";
	case 6U:
		return "data";
	default:
		return "other";
	}
}

std::uint32_t EncodePackFlags(bool dictionaryPresent) {
	return dictionaryPresent ? 1U : 0U;
}

bool HasDictionaryFlag(std::uint32_t flags) {
	// The low bit records the presence of shared dictionary bytes in the archive.
	return (flags & 1U) != 0U;
}

} // namespace tepak::archive
