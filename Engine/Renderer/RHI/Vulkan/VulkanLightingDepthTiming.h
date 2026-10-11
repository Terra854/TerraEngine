#pragma once
#include "Core/Export.h"
#include <array>
#include <cstdint>
#include <span>

namespace TerraEngine::Renderer::VulkanLightingDepthTiming
{
	inline constexpr std::uint32_t kFrameCount = 3, kSnapshotCount = 8, kPurposeCount = 2;
	inline constexpr std::uint32_t kQueryCount = kFrameCount * kSnapshotCount * kPurposeCount * 2;
	enum class Purpose : std::uint8_t { Production, Oracle };
	enum class View : std::uint8_t { Game, Scene };
	enum class Scope : std::uint8_t { IsolatedDepth, WholeView };
	enum class Status : std::uint8_t { Pending, Unavailable, Valid };
	enum class Reason : std::uint8_t { None, MissingMarkers, ReadFailed, MissingAvailability, ZeroNonemptyDuration, InvalidDuration, Canceled, Abandoned };
	struct Owner
	{
		std::uint64_t rendererSessionGeneration = 0, backendResourceGeneration = 0, queryPoolGeneration = 0;
		std::uint64_t frameSerial = 0, commandSerial = 0;
		std::uint32_t frameSlot = 0, snapshotSlot = 0, outputId = 0;
		Purpose purpose = Purpose::Production;
		View view = View::Game;
		Scope scope = Scope::IsolatedDepth;
		bool operator==(const Owner&) const = default;
	};
	struct QueryRange { std::uint32_t firstQuery = 0, count = 2; bool operator==(const QueryRange&) const = default; };
	struct QueryRead
	{
		std::uint64_t beginTicks = 0, endTicks = 0;
		bool beginAvailable = false, endAvailable = false, readSucceeded = false;
	};
	struct Result
	{
		Owner owner{};
		Status status = Status::Pending;
		Reason reason = Reason::None;
		std::uint64_t submitSerial = 0, completionSerial = 0, beginTicks = 0, endTicks = 0, rawTicks = 0;
		std::uint32_t timestampValidBits = 0;
		double timestampPeriodNanoseconds = 0, durationNanoseconds = 0;
		bool beginAvailable = false, endAvailable = false, nonemptyWork = false;
	};
	// One protected native owner drives this pure policy. It never calls Vulkan,
	// waits, allocates query pools, or infers completion from a frame-slot number.
	class TERRAENGINE_API TimingState
	{
	public:
		bool Configure(std::uint64_t session, std::uint64_t backend, std::uint64_t pool, std::uint32_t validBits, double period);
		bool Begin(const Owner& owner, QueryRange& range);
		bool End(const Owner& owner);
		bool ConfirmSubmitted(std::span<const std::uint64_t> orderedCommands, std::uint64_t submit);
		bool CompleteSubmission(std::uint64_t submit, std::uint64_t completion);
		bool Resolve(const Owner& owner, const QueryRead& queries, bool nonemptyWork, Result& result);
		bool AbandonCompleted(const Owner& owner, Result& result);
		bool DiscardCommand(std::uint64_t command);
		// Reset revokes proof immediately, but returns false while old physical
		// owners remain. The caller must retain their old query pool until drained.
		bool Reset(std::uint64_t session, std::uint64_t backend, std::uint64_t pool);
		std::uint32_t Outstanding() const;
		bool CanDestroy() const { return Outstanding() == 0; }
	private:
		struct Slot { Owner owner{}; Owner lastOwner{}; bool occupied = false, ended = false, canceled = false; std::uint64_t submit = 0, completion = 0; };
		std::array<Slot, kQueryCount / 2> slots{};
		std::uint64_t session = 0, backend = 0, pool = 0;
		std::uint32_t validBits = 0;
		double period = 0;
		bool accepting = false;
		Slot* Find(const Owner& owner);
		void Finish(Slot& slot, Result& result, Status status, Reason reason) const;
	};
	TERRAENGINE_API bool QualifiesProductionSpeedProof(const Result& result);
}
