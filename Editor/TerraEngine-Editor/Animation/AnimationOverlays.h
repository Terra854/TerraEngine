// AnimationOverlays.h - bounded immutable Scene View animation overlay packets.
//
// Collection is callback-based so disabled, collapsed, and renderer-unavailable
// paths can return without invoking expensive skeleton/pose/bounds extraction.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Editor::Animation {
	inline constexpr std::uint32_t kAnimationOverlaySchemaVersion = 1U;
	inline constexpr std::size_t kMaximumAnimationOverlayPrimitives = 8192U;
	inline constexpr std::size_t kMaximumAnimationOverlayCollectorPrimitives = 16384U;

	enum class AnimationOverlayStatus : std::uint8_t {
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

	enum class AnimationOverlayType : std::uint8_t {
		Skeleton = 0U,
		Socket,
		RootTrajectory,
		IkTarget,
		Contact,
		Ragdoll,
		MorphBounds,
		SkinnedBounds,
		VirtualGeometryCoverage
	};

	[[nodiscard]] constexpr std::uint32_t AnimationOverlayTypeMask(
		AnimationOverlayType type) noexcept {
		const std::uint32_t index = static_cast<std::uint32_t>(type);
		return index <= static_cast<std::uint32_t>(AnimationOverlayType::VirtualGeometryCoverage)
			? 1U << index : 0U;
	}

	inline constexpr std::uint32_t kAllAnimationOverlayKinds =
		(1U << (static_cast<std::uint32_t>(AnimationOverlayType::VirtualGeometryCoverage) + 1U)) - 1U;

	[[nodiscard]] constexpr std::string_view ToString(AnimationOverlayType type) noexcept {
		switch(type) {
		case AnimationOverlayType::Skeleton: return "Skeleton";
		case AnimationOverlayType::Socket: return "Socket";
		case AnimationOverlayType::RootTrajectory: return "Root trajectory";
		case AnimationOverlayType::IkTarget: return "IK target";
		case AnimationOverlayType::Contact: return "Contact";
		case AnimationOverlayType::Ragdoll: return "Ragdoll";
		case AnimationOverlayType::MorphBounds: return "Morph bounds";
		case AnimationOverlayType::SkinnedBounds: return "Skinned bounds";
		case AnimationOverlayType::VirtualGeometryCoverage: return "Virtual geometry coverage";
		}
		return "Unknown";
	}

	[[nodiscard]] constexpr std::string_view ToString(AnimationOverlayStatus status) noexcept {
		switch(status) {
		case AnimationOverlayStatus::Ok: return "Ok";
		case AnimationOverlayStatus::Disabled: return "Disabled";
		case AnimationOverlayStatus::Collapsed: return "Collapsed";
		case AnimationOverlayStatus::RendererUnavailable: return "RendererUnavailable";
		case AnimationOverlayStatus::InvalidArgument: return "InvalidArgument";
		case AnimationOverlayStatus::CapacityExceeded: return "CapacityExceeded";
		case AnimationOverlayStatus::Stale: return "Stale";
		case AnimationOverlayStatus::Rejected: return "Rejected";
		case AnimationOverlayStatus::InternalError: return "InternalError";
		}
		return "Unknown";
	}

	struct AnimationOverlayVec3 {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;

		auto operator<=>(const AnimationOverlayVec3&) const = default;
	};

	struct AnimationOverlayColor {
		float red = 1.0F;
		float green = 1.0F;
		float blue = 1.0F;
		float alpha = 1.0F;

		auto operator<=>(const AnimationOverlayColor&) const = default;
	};

	struct AnimationOverlayPrimitive {
		AnimationOverlayType type = AnimationOverlayType::Skeleton;
		std::uint64_t stableId = 0U;
		AnimationOverlayVec3 start;
		AnimationOverlayVec3 end;
		AnimationOverlayColor color;
		std::string label;
		std::string accessibilityText;

		auto operator<=>(const AnimationOverlayPrimitive&) const = default;
	};

	struct AnimationOverlayOptions {
		bool enabled = false;
		bool expanded = false;
		bool rendererAvailable = false;
		std::uint32_t enabledKinds = kAllAnimationOverlayKinds;
		std::size_t maximumPrimitives = kMaximumAnimationOverlayPrimitives;

		auto operator<=>(const AnimationOverlayOptions&) const = default;
	};

	struct AnimationOverlaySourceHeader {
		std::uint64_t sourceGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;

		auto operator<=>(const AnimationOverlaySourceHeader&) const = default;
	};

	struct AnimationOverlayWorkDeclaration {
		std::string_view phase = "UI/SceneView";
		std::string_view reads = "immutable animation, physics, and renderer diagnostic snapshots";
		std::string_view writes = "owned bounded Scene View overlay packet";
		std::string_view protectedBoundary = "PublishAnimationSceneOverlay";
		bool workerSafePreparation = true;
		bool protectedPublication = true;
		bool mutatesRuntime = false;

		auto operator<=>(const AnimationOverlayWorkDeclaration&) const = default;
	};

	struct AnimationOverlayInstrumentation {
		std::uint64_t collectionRequests = 0U;
		std::uint64_t copiedPrimitives = 0U;
		std::uint64_t filteredPrimitives = 0U;
		std::uint64_t droppedPrimitives = 0U;
		bool performanceTimingAttempted = false;
		bool resultSchemaValidated = true;

		auto operator<=>(const AnimationOverlayInstrumentation&) const = default;
	};

	struct AnimationOverlaySnapshot {
		std::uint32_t schemaVersion = kAnimationOverlaySchemaVersion;
		AnimationOverlayStatus status = AnimationOverlayStatus::Disabled;
		std::uint64_t settingsGeneration = 0U;
		std::uint64_t publicationGeneration = 0U;
		AnimationOverlaySourceHeader source;
		AnimationOverlayOptions options;
		std::vector<AnimationOverlayPrimitive> primitives;
		AnimationOverlayInstrumentation instrumentation;
		AnimationOverlayWorkDeclaration work;
		std::string accessibilityText = "Animation overlays disabled.";

		auto operator<=>(const AnimationOverlaySnapshot&) const = default;
	};

	struct AnimationOverlayPreparedPacket {
		std::uint32_t schemaVersion = kAnimationOverlaySchemaVersion;
		std::uint64_t expectedSettingsGeneration = 0U;
		AnimationOverlaySnapshot snapshot;
	};

	using AnimationOverlayCollector = std::function<AnimationOverlayStatus(
		std::uint32_t enabledKinds,
		std::size_t requestedMaximumPrimitives,
		std::vector<AnimationOverlayPrimitive>& output)>;

	class AnimationOverlays final {
	public:
		AnimationOverlays() = default;
		~AnimationOverlays();
		AnimationOverlays(const AnimationOverlays&) = delete;
		AnimationOverlays& operator=(const AnimationOverlays&) = delete;

		void Configure(AnimationOverlayOptions options) noexcept;
		[[nodiscard]] AnimationOverlayStatus Prepare(
			AnimationOverlaySourceHeader source,
			const AnimationOverlayCollector& collector,
			AnimationOverlayPreparedPacket& output) const noexcept;
		[[nodiscard]] AnimationOverlayStatus Publish(
			const AnimationOverlayPreparedPacket& prepared) noexcept;
		void Clear() noexcept;
		[[nodiscard]] AnimationOverlaySnapshot Snapshot() const noexcept;

	private:
		mutable std::mutex mutex_;
		AnimationOverlayOptions options_;
		std::uint64_t settingsGeneration_ = 1U;
		std::uint64_t publicationGeneration_ = 0U;
		AnimationOverlaySnapshot snapshot_;
	};
}
