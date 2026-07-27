// AnimationSceneHandles.h - bounded detached Scene View animation handles.
//
// The viewport consumes copied pose/goal data only. Editable handles keep a
// detached candidate until Apply queues the owning typed animation command, so
// pointer movement never mutates live animation or scene storage directly.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Editor::Animation {
	inline constexpr std::size_t kMaximumAnimationSceneHandles = 4096U;

	enum class AnimationHandleKind : std::uint8_t {
		IkGoal = 0U,
		Socket = 1U,
		JointLimit = 2U,
		Contact = 3U,
		Joint = 4U,
		RagdollBody = 5U,
		RootMotion = 6U
	};

	struct AnimationSceneHandle {
		AnimationHandleKind kind{};
		// ownerId is the durable goal, joint, socket, or profile record identity.
		std::uint64_t ownerId = 0U;
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
		// Optional line endpoints let the same bounded packet represent a bone,
		// trajectory, constraint axis, or contact normal without renderer pointers.
		float endX = 0.0F;
		float endY = 0.0F;
		float endZ = 0.0F;
		float rotationX = 0.0F;
		float rotationY = 0.0F;
		float rotationZ = 0.0F;
		float rotationW = 1.0F;
		float weight = 1.0F;
		std::uint64_t sourceGeneration = 0U;
		std::string label;
		bool hasEndPoint = false;
		bool enabled = true;
		bool editable = false;

		auto operator<=>(const AnimationSceneHandle&) const = default;
	};

	struct AnimationSceneHandleEdit {
		AnimationSceneHandle before;
		AnimationSceneHandle candidate;
		std::uint64_t expectedSourceGeneration = 0U;
	};

	class AnimationSceneHandles final {
	public:
		// Set validates the complete candidate before replacing the visible packet.
		// The previous packet survives allocation or validation failure atomically.
		bool Set(std::vector<AnimationSceneHandle> handles) noexcept;
		[[nodiscard]] const std::vector<AnimationSceneHandle>& Snapshot() const noexcept {
			return handles_;
		}

		bool Select(AnimationHandleKind kind, std::uint64_t ownerId) noexcept;
		void ClearSelection() noexcept;
		[[nodiscard]] const AnimationSceneHandle* Selected() const noexcept;
		[[nodiscard]] bool BeginEdit(std::uint64_t expectedSourceGeneration) noexcept;
		[[nodiscard]] bool UpdateEditPosition(float x, float y, float z) noexcept;
		[[nodiscard]] bool UpdateEditRotation(float x, float y, float z, float w) noexcept;
		[[nodiscard]] bool CommitEdit(
			std::uint64_t currentSourceGeneration,
			AnimationSceneHandle& output) noexcept;
		void CancelEdit() noexcept { edit_.reset(); }
		[[nodiscard]] const AnimationSceneHandleEdit* ActiveEdit() const noexcept {
			return edit_ ? &*edit_ : nullptr;
		}

		[[nodiscard]] bool RendererAvailable() const noexcept { return rendererAvailable_; }
		void SetRendererAvailable(bool available) noexcept { rendererAvailable_ = available; }

	private:
		[[nodiscard]] static bool IsFinite(const AnimationSceneHandle& handle) noexcept;

		std::vector<AnimationSceneHandle> handles_;
		std::optional<std::pair<AnimationHandleKind, std::uint64_t>> selection_;
		std::optional<AnimationSceneHandleEdit> edit_;
		bool rendererAvailable_ = false;
	};
}
