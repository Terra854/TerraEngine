// PostProcessDiagnostics.h - Effect status and completed GPU timing evidence.
//
// A disabled or unavailable effect has an explicit status and no numeric cost.
// The Vulkan recorder reserves a timestamp range only when it records real
// effect work. A measured duration belongs to the exact submitted command,
// output lifetime, settings revision, and completed query-pool generation.
#pragma once

#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Renderer
{
	enum class PostProcessPassKind;
}

namespace TerraEngine::Renderer::GraphicsSettings
{
	enum class PostProcessEffectState : std::uint32_t
	{
		Active = 0,
		Disabled = 1,
		Rejected = 2,
		Unavailable = 3
	};

	enum class PostProcessTimingAvailability : std::uint32_t
	{
		NotRequested = 0,
		Pending = 1,
		Measured = 2,
		Unavailable = 3,
		Invalid = 4,
		Cancelled = 5,
		Abandoned = 6
	};

	enum class PostProcessDiagnosticReason : std::uint32_t
	{
		None = 0,
		Disabled = 1,
		UnsupportedDevice = 2,
		MissingResources = 3,
		InvalidConfiguration = 4,
		QueryCapacityExceeded = 5,
		MissingMarkers = 6,
		MissingAvailability = 7,
		QueryReadFailed = 8,
		InvalidDuration = 9,
		StaleLifetime = 10,
		DiscardedCommand = 11,
		AbandonedReadback = 12
	};

	// The registry supplies a stable built-in name or project asset identifier.
	// The graph stage is separate because one stage may contain several effects.
	struct PostProcessEffectIdentity
	{
		std::string stableId;
		TerraEngine::Renderer::PostProcessPassKind passKind = static_cast<TerraEngine::Renderer::PostProcessPassKind>(0);
		bool operator==(const PostProcessEffectIdentity&) const = default;
	};

	// A frame slot locates physical query storage but never proves completion.
	// Command, session, backend, pool, and view generations identify its owner.
	struct PostProcessTimingOwner
	{
		GraphicsLifecycleStamp lifecycle{};
		GraphicsViewIdentity view{};
		// A game-scoped settings root may have no camera/view lifetime stamp.
		// The actual executable target still provides exact resource ownership.
		std::uint64_t targetGeneration = 0;
		GraphicsRevision settingsRevision = 0;
		std::uint64_t frameId = 0;
		std::uint64_t commandSerial = 0;
		std::uint64_t queryPoolGeneration = 0;
		std::uint32_t frameSlot = 0;
		std::uint32_t snapshotSlot = 0;
		bool operator==(const PostProcessTimingOwner&) const = default;
	};

	struct PostProcessTimestampRange
	{
		std::uint32_t firstQuery = 0;
		std::uint32_t queryCount = 2;
		bool operator==(const PostProcessTimestampRange&) const = default;
	};

	// Numeric duration is populated only after accepted submission, completed
	// fence ownership, and available query values. Invalid and abandoned results
	// retain their provenance and reason while duration stays absent.
	struct PostProcessTimingSample
	{
		PostProcessEffectIdentity effect{};
		PostProcessTimingOwner owner{};
		PostProcessTimestampRange queries{};
		PostProcessTimingAvailability availability = PostProcessTimingAvailability::NotRequested;
		PostProcessDiagnosticReason reason = PostProcessDiagnosticReason::None;
		std::uint64_t submitSerial = 0;
		std::uint64_t completionSerial = 0;
		std::uint64_t beginTicks = 0;
		std::uint64_t endTicks = 0;
		std::uint64_t elapsedTicks = 0;
		std::uint32_t timestampValidBits = 0;
		double timestampPeriodNanoseconds = 0;
		std::optional<double> durationMilliseconds;
		bool beginAvailable = false;
		bool endAvailable = false;
		bool nonemptyWork = false;
	};

	struct PostProcessEffectDiagnostic
	{
		PostProcessEffectIdentity effect{};
		PostProcessEffectState state = PostProcessEffectState::Disabled;
		PostProcessDiagnosticReason reason = PostProcessDiagnosticReason::Disabled;
		GraphicsRequestId requestId = 0;
		std::string detail;
		PostProcessTimingSample timing{};
	};

	// This owned snapshot can cross renderer, native diagnostics, managed query,
	// and editor automation boundaries. Delayed samples retain their own frame
	// identity and cannot be reattributed to the frame displaying the diagnostics.
	struct PostProcessDiagnosticsSnapshot
	{
		GraphicsLifecycleStamp lifecycle{};
		GraphicsViewIdentity view{};
		GraphicsRevision settingsRevision = 0;
		std::uint64_t frameId = 0;
		std::vector<PostProcessEffectDiagnostic> effects;
	};

	// The evidence consumer verifies all ownership and timing facts rather than
	// interpreting an unavailable row's default values as a zero-cost effect.
	[[nodiscard]] inline bool HasMeasuredPostProcessGpuDuration(const PostProcessTimingSample& sample) noexcept
	{
		return sample.availability == PostProcessTimingAvailability::Measured
			&& sample.reason == PostProcessDiagnosticReason::None
			&& !sample.effect.stableId.empty()
			&& sample.owner.lifecycle.rendererSessionGeneration != 0
			&& sample.owner.lifecycle.backendResourceGeneration != 0
			&& sample.owner.targetGeneration != 0
			&& sample.owner.settingsRevision != 0
			&& sample.owner.frameId != 0
			&& sample.owner.commandSerial != 0
			&& sample.owner.queryPoolGeneration != 0
			&& sample.queries.queryCount == 2
			&& sample.submitSerial != 0
			&& sample.completionSerial >= sample.submitSerial
			&& sample.beginAvailable && sample.endAvailable && sample.nonemptyWork
			&& sample.elapsedTicks != 0
			&& sample.timestampValidBits > 0 && sample.timestampValidBits <= 64
			&& std::isfinite(sample.timestampPeriodNanoseconds)
			&& sample.timestampPeriodNanoseconds > 0
			&& sample.durationMilliseconds.has_value()
			&& std::isfinite(*sample.durationMilliseconds)
			&& *sample.durationMilliseconds > 0;
	}
}
