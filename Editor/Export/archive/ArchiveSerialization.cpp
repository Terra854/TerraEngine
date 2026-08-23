// ArchiveSerialization.cpp — TEPAK archive serialization implementation
//
// Implements the conversion and encoding rules for TEPAK archive metadata. It provides helpers for mapping asset classes to compact numeric codes and managing archive-level flags like shared dictionary presence.

#include "archive/ArchiveWorkflow.h"

#include <unordered_map>

namespace tepak::archive {

// EncodeAssetClass is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
		// Animation classes are append-only TEPAK identities. Keep these strings
		// aligned with export dependency manifests and the runtime decoder.
		{"skeleton", 8U},
		{"animation-clip", 9U},
		{"animation-controller", 10U},
		{"animation-mask", 11U},
		{"retarget-profile", 12U},
		{"motion-database", 13U},
		{"ragdoll-profile", 14U},
		// Terrain uses the same append-only code as the runtime reader. If the
		// writer falls back to "other", checked ContentMount reads reject the
		// descriptor even though its bytes are present in the archive.
		{"terrain", 15U},
	};

	const auto found = kCodes.find(assetClass);
	return found == kCodes.end() ? 7U : found->second;
}

// DecodeAssetClass is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string DecodeAssetClass(std::uint32_t assetClassCode) {
	// Unknown codes intentionally degrade to "other" so older tools can still
	// inspect newer archives without crashing on an enum mismatch.
	// Dispatch by explicit engine state so each case documents one supported behavior.
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
	case 8U:
		return "skeleton";
	case 9U:
		return "animation-clip";
	case 10U:
		return "animation-controller";
	case 11U:
		return "animation-mask";
	case 12U:
		return "retarget-profile";
	case 13U:
		return "motion-database";
	case 14U:
		return "ragdoll-profile";
	case 15U:
		return "terrain";
	default:
		return "other";
	}
}

// EncodePackFlags is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint32_t EncodePackFlags(bool dictionaryPresent) {
	return dictionaryPresent ? 1U : 0U;
}

// HasDictionaryFlag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool HasDictionaryFlag(std::uint32_t flags) {
	// The low bit records the presence of shared dictionary bytes in the archive.
	return (flags & 1U) != 0U;
}

} // namespace tepak::archive
