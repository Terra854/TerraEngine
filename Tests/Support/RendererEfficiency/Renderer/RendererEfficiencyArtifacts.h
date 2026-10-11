// Owned validation payloads contain every native pixel, never a sampled proxy.
#pragma once
#include "Renderer/ShadowWorkPlan.h"
#include "Renderer/LightingDepthEvidence.h"
#include <memory>
#include <vector>
#include <span>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <bit>
#include <optional>

namespace TerraEngine::Renderer
{
	// Occupancy is a byte for every native pixel. It proves raster coverage;
	// exact caster identity is separately retained in the segment sidecar.
	inline bool DecodeShadowDepthCoverage(std::span<const std::uint8_t> bytes, std::uint32_t width,
		std::uint32_t height, std::vector<std::uint8_t>& coverage)
	{
		if constexpr (std::endian::native != std::endian::little) return false;
		const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
		if (!pixels || pixels > bytes.size() / 4 || bytes.size() % 4 || pixels != bytes.size() / 4) return false;
		std::vector<std::uint8_t> result(static_cast<std::size_t>(pixels));
		for (std::size_t pixel = 0; pixel < result.size(); ++pixel)
		{
			float depth; std::memcpy(&depth, bytes.data() + pixel * 4, 4);
			if (!std::isfinite(depth) || depth < 0 || depth > 1) return false;
			result[pixel] = depth < 1.0f ? 1 : 0;
		}
		coverage = std::move(result); return true;
	}
	struct RendererEfficiencyFrameColor
	{
		std::uint32_t width = 0, height = 0, sourceFormat = 0;
		std::vector<std::uint8_t> bytes; // Native tightly packed color; host export encodes PNG.
	};
	struct RendererEfficiencyArtifacts
	{
		// The completed collector owns both real production bounds and optional
		// isolated oracle readbacks. A reservation alone never fills this pointer.
		std::shared_ptr<const RendererEfficiencyLightingDepthArtifacts> lightingDepth;
		// Lighting-only validation copies the real GPU buffer slice after submit
		// completion, including its zero inactive tail. It shares native frame color.
		std::vector<std::uint8_t> lightingPayload;
		// Copied before native light budgeting: GPU record count alone cannot
		// prove that overflow candidates were genuinely present in the source.
		std::optional<std::uint32_t> fullSourceCandidateCount;
		ShadowViewKey shadowKey{};
		ShadowCasterMembership eligibleMembership;
		std::vector<ShadowCasterStamp> submittedSegments;
		std::uint64_t contentToken = 0;
		std::uint32_t depthSourceFormat = 0;
		std::string depthEncoding = "float32-le", coverageEncoding = "occupied-u8";
		std::vector<std::uint8_t> depthF32LE, coverageU8;
		std::shared_ptr<const RendererEfficiencyFrameColor> frameColor;
	};
	// This pure receipt gate has no Vulkan handles. A reservation is not GPU
	// evidence; only the backend's real submit and fence receipt can advance it.
	class ShadowValidationReceiptTracker
	{
	public:
		struct Row { std::uint64_t id, command, bytes, submit = 0; bool completed = false; };
		explicit ShadowValidationReceiptTracker(std::uint64_t bytes = 512ULL * 1024 * 1024, std::size_t count = 256) : byteLimit(bytes), countLimit(count) {}
		bool Reserve(std::uint64_t id, std::uint64_t command, std::uint64_t bytes)
		{
			if (!id || !command || !bytes || rows.size() >= countLimit || bytes > byteLimit - reservedBytes
				|| std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; })) return false;
			rows.push_back({id, command, bytes}); reservedBytes += bytes; return true;
		}
		bool Confirm(std::span<const std::uint64_t> commands, std::uint64_t submit)
		{
			if (commands.empty() || !submit || submit < lastSubmit || submit <= lastConfirmedSubmit) return false;
			bool found = false;
			for (std::size_t i = 0; i < commands.size(); ++i)
			{
				if (!commands[i] || std::find(commands.begin(), commands.begin() + i, commands[i]) != commands.begin() + i) return false;
				for (const auto& row : rows) if (row.command == commands[i]) { if (row.submit && row.submit != submit) return false; found = true; }
			}
			if (!found) return false; // Shadow-free actual primaries may accompany relevant ones.
			for (auto& row : rows) if (std::find(commands.begin(), commands.end(), row.command) != commands.end()) row.submit = submit;
			lastSubmit = submit; lastConfirmedSubmit = submit; return true;
		}
		// Called only after real queue success, before any allocating receipt work.
		// Even if later batching fails, this row is still owned by the GPU.
		void PreserveSubmittedCommand(std::uint64_t command, std::uint64_t submit) noexcept
		{
			if (!command || !submit || submit < lastSubmit) return;
			for (auto& row : rows) if (row.command == command && (!row.submit || row.submit == submit)) row.submit = submit;
			lastSubmit = submit;
		}
		std::vector<std::uint64_t> Complete(std::uint64_t submit, std::uint64_t completion)
		{
			std::vector<std::uint64_t> result;
			if (!submit || completion < submit) return result;
			// Retain completed IDs until Release: host allocation failure may retry
			// the copy, while the backend fence receipt remains authoritative.
			for (auto& row : rows) if (row.submit == submit) { result.push_back(row.id); row.completed = true; }
			return result;
		}
		std::vector<std::uint64_t> Discard(std::uint64_t command)
		{
			std::vector<std::uint64_t> result;
			for (const auto& row : rows) if (row.command == command && !row.submit) result.push_back(row.id);
			for (auto id : result) Release(id);
			return result;
		}
		bool Release(std::uint64_t id)
		{
			const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.id == id; });
			if (row == rows.end() || (row->submit && !row->completed)) return false;
			reservedBytes -= row->bytes; rows.erase(row); return true;
		}
		std::uint64_t ReservedBytes() const { return reservedBytes; }
		std::size_t Count() const { return rows.size(); }
	private:
		std::uint64_t byteLimit, reservedBytes = 0, lastSubmit = 0, lastConfirmedSubmit = 0;
		std::size_t countLimit;
		std::vector<Row> rows;
	};
}
