#pragma once
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <cstdint>

namespace tepak::io {
	// Preflight the complete selection before output creation. Retained directory
	// anchors prevent a pathname from redirecting worker publication elsewhere.
	class ExtractionDestination final {
	public:
		ExtractionDestination(const std::filesystem::path& root, std::span<const std::string> selectedPaths);
		~ExtractionDestination();
		ExtractionDestination(const ExtractionDestination&) = delete;
		ExtractionDestination& operator=(const ExtractionDestination&) = delete;
		// Bytes are already decoded and verified; publication replaces a regular
		// destination atomically without opening that destination for truncation.
		void Publish(std::string_view relativePath, std::span<const std::uint8_t> bytes) const;
	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
