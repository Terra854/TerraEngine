// AnimationDiagnosticsWindow.h - bounded immutable animation diagnostics UI.
//
// Detailed collection is an explicit callback that Prepare invokes only while
// diagnostics are enabled and expanded. Prepare owns copies and may run on a
// worker; Publish is the sole protected presentation boundary.

#pragma once

#include "../Framework/EditorWindow.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Editor::Animation {
	inline constexpr std::uint32_t kAnimationDiagnosticsEditorSchemaVersion = 1U;
	inline constexpr std::size_t kMaximumAnimationDiagnosticsEditorRows = 256U;
	inline constexpr std::size_t kMaximumAnimationDiagnosticsCollectorRows = 1024U;

	enum class AnimationDiagnosticsStatus : std::uint8_t {
		Ok = 0U,
		Disabled,
		Collapsed,
		RendererUnavailable,
		InvalidArgument,
		CapacityExceeded,
		Stale,
		Rejected,
		InternalError
	};

	enum class AnimationDiagnosticsCategory : std::uint8_t {
		Profiler = 0U,
		State,
		Pose,
		Event,
		RootMotion,
		Ik,
		Retarget,
		MotionMatching,
		Ragdoll,
		Bounds,
		Residency,
		Mode
	};

	[[nodiscard]] constexpr std::uint32_t AnimationDiagnosticsCategoryMask(
		AnimationDiagnosticsCategory category) noexcept {
		const std::uint32_t index = static_cast<std::uint32_t>(category);
		return index <= static_cast<std::uint32_t>(AnimationDiagnosticsCategory::Mode)
			? 1U << index : 0U;
	}

	inline constexpr std::uint32_t kAllAnimationDiagnosticsCategories =
		(1U << (static_cast<std::uint32_t>(AnimationDiagnosticsCategory::Mode) + 1U)) - 1U;

	[[nodiscard]] constexpr std::string_view ToString(
		AnimationDiagnosticsCategory category) noexcept {
		switch(category) {
		case AnimationDiagnosticsCategory::Profiler: return "Profiler";
		case AnimationDiagnosticsCategory::State: return "State";
		case AnimationDiagnosticsCategory::Pose: return "Pose";
		case AnimationDiagnosticsCategory::Event: return "Event";
		case AnimationDiagnosticsCategory::RootMotion: return "Root Motion";
		case AnimationDiagnosticsCategory::Ik: return "IK";
		case AnimationDiagnosticsCategory::Retarget: return "Retarget";
		case AnimationDiagnosticsCategory::MotionMatching: return "Motion Matching";
		case AnimationDiagnosticsCategory::Ragdoll: return "Ragdoll";
		case AnimationDiagnosticsCategory::Bounds: return "Bounds";
		case AnimationDiagnosticsCategory::Residency: return "Residency";
		case AnimationDiagnosticsCategory::Mode: return "Mode";
		}
		return "Unknown";
	}

	[[nodiscard]] constexpr std::string_view ToString(
		AnimationDiagnosticsStatus status) noexcept {
		switch(status) {
		case AnimationDiagnosticsStatus::Ok: return "Ok";
		case AnimationDiagnosticsStatus::Disabled: return "Disabled";
		case AnimationDiagnosticsStatus::Collapsed: return "Collapsed";
		case AnimationDiagnosticsStatus::RendererUnavailable: return "RendererUnavailable";
		case AnimationDiagnosticsStatus::InvalidArgument: return "InvalidArgument";
		case AnimationDiagnosticsStatus::CapacityExceeded: return "CapacityExceeded";
		case AnimationDiagnosticsStatus::Stale: return "Stale";
		case AnimationDiagnosticsStatus::Rejected: return "Rejected";
		case AnimationDiagnosticsStatus::InternalError: return "InternalError";
		}
		return "Unknown";
	}

	struct AnimationDiagnosticsOptions {
		bool enabled = false;
		bool collapsed = true;
		bool rendererAvailable = false;
		std::uint32_t enabledCategories = kAllAnimationDiagnosticsCategories;
		std::size_t maximumRows = kMaximumAnimationDiagnosticsEditorRows;

		auto operator<=>(const AnimationDiagnosticsOptions&) const = default;
	};

	struct AnimationDiagnosticsRow {
		AnimationDiagnosticsCategory category = AnimationDiagnosticsCategory::Profiler;
		std::uint64_t stableInstanceId = 0U;
		std::string label;
		std::string value;
		std::string accessibilityText;
		bool rendererDependent = false;

		auto operator<=>(const AnimationDiagnosticsRow&) const = default;
	};

	struct AnimationDiagnosticsWorkDeclaration {
		std::string_view phase = "UI/Diagnostics";
		std::string_view reads = "immutable animation summary and opt-in copied detailed records";
		std::string_view writes = "owned presentation snapshot";
		std::string_view protectedBoundary = "PublishAnimationDiagnosticsWindow";
		bool workerSafePreparation = true;
		bool protectedPublication = true;
		bool mutatesRuntime = false;

		auto operator<=>(const AnimationDiagnosticsWorkDeclaration&) const = default;
	};

	// These are correctness counters, not measured performance evidence. They let
	// intermediate tests prove disabled/collapsed paths requested zero expensive
	// records while ImplementationStage remains the only timing-acceptance phase.
	struct AnimationDiagnosticsInstrumentation {
		std::uint64_t summaryCopies = 0U;
		std::uint64_t detailedCollectionRequests = 0U;
		std::uint64_t rowsCopied = 0U;
		std::uint64_t rowsDropped = 0U;
		std::uint64_t rendererRowsSuppressed = 0U;
		bool performanceTimingAttempted = false;
		bool resultSchemaValidated = true;

		auto operator<=>(const AnimationDiagnosticsInstrumentation&) const = default;
	};

	struct AnimationDiagnosticsWindowSnapshot {
		std::uint32_t schemaVersion = kAnimationDiagnosticsEditorSchemaVersion;
		AnimationDiagnosticsStatus status = AnimationDiagnosticsStatus::Disabled;
		std::uint64_t sourceGeneration = 0U;
		std::uint64_t settingsGeneration = 0U;
		std::uint64_t publicationGeneration = 0U;
		AnimationDiagnosticsOptions options;
		TerraEngine::Animation::AnimationDiagnosticsSnapshot summary;
		bool summaryAvailable = false;
		std::vector<AnimationDiagnosticsRow> rows;
		AnimationDiagnosticsInstrumentation instrumentation;
		AnimationDiagnosticsWorkDeclaration work;
		std::string accessibilityText = "Animation diagnostics disabled.";
	};

	struct AnimationDiagnosticsPreparedSnapshot {
		std::uint32_t schemaVersion = kAnimationDiagnosticsEditorSchemaVersion;
		std::uint64_t expectedSettingsGeneration = 0U;
		AnimationDiagnosticsWindowSnapshot snapshot;
	};

	using AnimationDiagnosticsCollector = std::function<AnimationDiagnosticsStatus(
		std::uint32_t enabledCategories,
		std::size_t requestedMaximumRows,
		std::vector<AnimationDiagnosticsRow>& output)>;

	class AnimationDiagnosticsPresentationModel final {
	public:
		AnimationDiagnosticsPresentationModel() = default;
		AnimationDiagnosticsPresentationModel(const AnimationDiagnosticsPresentationModel&) = delete;
		AnimationDiagnosticsPresentationModel& operator=(const AnimationDiagnosticsPresentationModel&) = delete;

		void Configure(AnimationDiagnosticsOptions options) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				options.maximumRows = std::clamp<std::size_t>(
					options.maximumRows, 1U, kMaximumAnimationDiagnosticsEditorRows);
				options.enabledCategories &= kAllAnimationDiagnosticsCategories;
				options_ = options;
				if(settingsGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
					++settingsGeneration_;
				}
			} catch(...) {
				// Configuration has no external side effect. The prior complete options
				// remain active if the platform mutex reports a failure.
			}
		}

		[[nodiscard]] AnimationDiagnosticsStatus Prepare(
			const TerraEngine::Animation::AnimationDiagnosticsSnapshot& summary,
			std::uint64_t sourceGeneration,
			const AnimationDiagnosticsCollector& collector,
			AnimationDiagnosticsPreparedSnapshot& output) const noexcept {
			try {
				AnimationDiagnosticsOptions options;
				std::uint64_t settingsGeneration = 0U;
				{
					std::scoped_lock lock(mutex_);
					options = options_;
					settingsGeneration = settingsGeneration_;
				}

				AnimationDiagnosticsPreparedSnapshot staged;
				staged.expectedSettingsGeneration = settingsGeneration;
				staged.snapshot.settingsGeneration = settingsGeneration;
				staged.snapshot.sourceGeneration = sourceGeneration;
				staged.snapshot.options = options;
				if(!options.enabled) {
					staged.snapshot.status = AnimationDiagnosticsStatus::Disabled;
					staged.snapshot.accessibilityText = "Animation diagnostics disabled; detailed collection is inactive.";
					output = std::move(staged);
					return AnimationDiagnosticsStatus::Disabled;
				}
				if(options.collapsed) {
					staged.snapshot.status = AnimationDiagnosticsStatus::Collapsed;
					staged.snapshot.accessibilityText = "Animation diagnostics collapsed; detailed collection is inactive.";
					output = std::move(staged);
					return AnimationDiagnosticsStatus::Collapsed;
				}
				if(sourceGeneration == 0U
					|| summary.schemaVersion != 1U
					|| !collector
					|| options.enabledCategories == 0U) {
					return AnimationDiagnosticsStatus::InvalidArgument;
				}

				staged.snapshot.summary = summary;
				staged.snapshot.summaryAvailable = true;
				staged.snapshot.instrumentation.summaryCopies = 1U;
				staged.snapshot.instrumentation.detailedCollectionRequests = 1U;
				std::vector<AnimationDiagnosticsRow> collected;
				const AnimationDiagnosticsStatus collectedStatus = collector(
					options.enabledCategories,
					options.maximumRows,
					collected);
				if(collectedStatus != AnimationDiagnosticsStatus::Ok) {
					return collectedStatus;
				}
				if(collected.size() > kMaximumAnimationDiagnosticsCollectorRows) {
					return AnimationDiagnosticsStatus::CapacityExceeded;
				}

				staged.snapshot.rows.reserve(std::min(collected.size(), options.maximumRows));
				for(AnimationDiagnosticsRow& row : collected) {
					const std::uint32_t category = AnimationDiagnosticsCategoryMask(row.category);
					if((category & kAllAnimationDiagnosticsCategories) == 0U
						|| row.label.empty() || row.value.empty()
						|| row.label.size() > 256U || row.value.size() > 512U
						|| row.accessibilityText.size() > 768U) {
						return AnimationDiagnosticsStatus::InvalidArgument;
					}
					if((options.enabledCategories & category) == 0U) {
						++staged.snapshot.instrumentation.rowsDropped;
						continue;
					}
					if(!options.rendererAvailable && row.rendererDependent) {
						++staged.snapshot.instrumentation.rendererRowsSuppressed;
						continue;
					}
					if(row.accessibilityText.empty()) {
						row.accessibilityText = std::string(ToString(row.category))
							+ ", " + row.label + ": " + row.value;
					}
					if(staged.snapshot.rows.size() == options.maximumRows) {
						++staged.snapshot.instrumentation.rowsDropped;
						continue;
					}
					staged.snapshot.rows.push_back(std::move(row));
					++staged.snapshot.instrumentation.rowsCopied;
				}

				staged.snapshot.status = options.rendererAvailable
					? AnimationDiagnosticsStatus::Ok
					: AnimationDiagnosticsStatus::RendererUnavailable;
				staged.snapshot.accessibilityText = options.rendererAvailable
					? "Animation diagnostics expanded with immutable copied runtime data."
					: "Animation diagnostics: renderer unavailable; CPU-authoritative data remains available.";
				output = std::move(staged);
				return output.snapshot.status;
			} catch(...) {
				return AnimationDiagnosticsStatus::InternalError;
			}
		}

		[[nodiscard]] AnimationDiagnosticsStatus Publish(
			const AnimationDiagnosticsPreparedSnapshot& prepared) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				if(prepared.schemaVersion != kAnimationDiagnosticsEditorSchemaVersion
					|| prepared.snapshot.schemaVersion != kAnimationDiagnosticsEditorSchemaVersion) {
					return AnimationDiagnosticsStatus::InvalidArgument;
				}
				if(prepared.expectedSettingsGeneration != settingsGeneration_) {
					return AnimationDiagnosticsStatus::Stale;
				}
				AnimationDiagnosticsWindowSnapshot staged = prepared.snapshot;
				if(publicationGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
					++publicationGeneration_;
				}
				staged.publicationGeneration = publicationGeneration_;
				snapshot_ = std::move(staged);
				return snapshot_.status;
			} catch(...) {
				return AnimationDiagnosticsStatus::InternalError;
			}
		}

		void Close() noexcept {
			try {
				std::scoped_lock lock(mutex_);
				options_.enabled = false;
				if(settingsGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
					++settingsGeneration_;
				}
				if(publicationGeneration_ != std::numeric_limits<std::uint64_t>::max()) {
					++publicationGeneration_;
				}
				snapshot_ = {};
				snapshot_.status = AnimationDiagnosticsStatus::Disabled;
				snapshot_.settingsGeneration = settingsGeneration_;
				snapshot_.publicationGeneration = publicationGeneration_;
				snapshot_.options = options_;
				snapshot_.accessibilityText = "Animation diagnostics closed; detailed collection is inactive.";
			} catch(...) {
			}
		}

		[[nodiscard]] AnimationDiagnosticsWindowSnapshot Snapshot() const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				return snapshot_;
			} catch(...) {
				return {};
			}
		}

	private:
		mutable std::mutex mutex_;
		AnimationDiagnosticsOptions options_;
		std::uint64_t settingsGeneration_ = 1U;
		std::uint64_t publicationGeneration_ = 0U;
		AnimationDiagnosticsWindowSnapshot snapshot_;
	};
}

// The window owns presentation only. Runtime systems publish copied snapshots;
// MainEditorSubsystem or automation configures this model without giving ImGui
// access to animator instances, renderer allocations, or producer callbacks.
class AnimationDiagnosticsWindow final : public EditorWindow {
public:
	AnimationDiagnosticsWindow();
	~AnimationDiagnosticsWindow() override;

	// RefreshPublishedSnapshots copies the latest protected runtime publications
	// into this window's owned presentation model. It is public so editor tests
	// can exercise the exact production collection path without constructing an
	// ImGui frame or borrowing any animator/runtime storage.
	[[nodiscard]] TerraEngine::Editor::Animation::AnimationDiagnosticsStatus
		RefreshPublishedSnapshots(bool rendererAvailable) noexcept;

	[[nodiscard]] TerraEngine::Editor::Animation::AnimationDiagnosticsPresentationModel& Model() noexcept {
		return model_;
	}

	[[nodiscard]] const TerraEngine::Editor::Animation::AnimationDiagnosticsPresentationModel& Model() const noexcept {
		return model_;
	}

protected:
	void OnGui() override;
	bool OnCloseRequested() override;

private:
	TerraEngine::Editor::Animation::AnimationDiagnosticsPresentationModel model_;
	// The visible window owns one scoped runtime request. Other consumers retain
	// independent leases, so closing this window cannot suppress an in-flight MCP
	// detailed request.
	TerraEngine::Animation::AnimationDetailedDiagnosticsRequest detailedRequest_;
};
