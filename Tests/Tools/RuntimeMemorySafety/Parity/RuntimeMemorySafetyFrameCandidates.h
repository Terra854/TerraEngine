#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace TerraEngine::Validation::RuntimeMemorySafety {
	// Own only paths generated here, never filenames taken from receipt JSON.
	// Artifact identities keep increasing after pruning; observedOrdinal instead
	// describes the current receipt's contiguous order and is rebuilt on retirement.
	class ParityFrameArtifactRows {
	public:
		explicit ParityFrameArtifactRows(std::filesystem::path root) : root_(std::move(root)) {}
		std::array<std::filesystem::path, 3> Allocate(std::uint64_t frame) {
			if (nextIdentity_ == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("Parity artifact identity exhausted");
			const auto prefix = "row-" + std::to_string(nextIdentity_++);
			std::array<std::filesystem::path, 3> paths{root_ / (prefix + ".lighting.bin"),
				root_ / (prefix + ".color.bin"), root_ / (prefix + ".rgba8.bin")};
			files_[frame].push_back(paths);
			return paths;
		}
		template<class Rows>
		void Retire(std::uint64_t frame, Rows& rows) {
			if (const auto found = files_.find(frame); found != files_.end()) {
				for (const auto& paths : found->second)
					for (const auto& path : paths)
						if (!std::filesystem::remove(path))
							throw std::runtime_error("Owned rolling parity binary could not be retired");
				files_.erase(found);
			}
			for (auto row = rows.begin(); row != rows.end();)
				if (row->at("frameSerial").template get<std::uint64_t>() == frame) row = rows.erase(row);
				else ++row;
			std::size_t ordinal = 0;
			for (auto& row : rows) row["observedOrdinal"] = ordinal++;
		}
	private:
		std::filesystem::path root_;
		std::uint64_t nextIdentity_ = 0;
		std::map<std::uint64_t, std::vector<std::array<std::filesystem::path, 3>>> files_;
	};
	// Release the oldest owned candidate BEFORE admitting a new capture. A late
	// metadata join can then select any of the most recent sixteen exact frames.
	// The callback owns disk retirement; the CPU-only overload owns no files.
	template<class CandidateMap, class Retire>
	bool CanCaptureParityFrameCandidate(CandidateMap& outputs, Retire retire) {
		while (outputs.size() >= 16U) {
			auto oldest = outputs.begin();
			retire(oldest->first, oldest->second);
			outputs.erase(oldest);
		}
		return true;
	}
	template<class CandidateMap>
	bool CanCaptureParityFrameCandidate(CandidateMap& outputs) {
		return CanCaptureParityFrameCandidate(outputs, [](const auto&, const auto&) {});
	}
	// Preserve exact native frame/purpose membership; never join by elapsed host
	// iteration, camera ordinal alone, or a receipt from a different frame.
	template<class MetadataMap, class CommandMap, class CandidateMap>
	std::uint64_t SelectCompleteParityFrameCandidate(const MetadataMap& metadata,
		CommandMap& commands, const CandidateMap& outputs, std::size_t expected) {
		for (const auto& [frame, rows] : metadata)
			if (rows.size() == 2U * expected && !commands[frame].empty()
				&& commands[frame].size() <= expected && outputs.contains(frame)) return frame;
		return 0U;
	}
}
