// Durable post-process volume authoring. No renderer resources or live profile
// pointers are stored here; protected scene capture resolves/copies those later.
#pragma once
#include "Core/Object/Component.h"
#include <array>
#include <cstdint>
#include <string_view>

struct PostProcessVolumeComponent final : Component<PostProcessVolumeComponent>
{
	static constexpr std::string_view kComponentName = "PostProcessVolumeComponent";
	// Opt into the existing detached component transaction path. This value-only
	// component has no callbacks, external ownership or publication side effects;
	// retained copies can be restored with no-throw moves on batch rollback.
	static constexpr bool kTransactionalAuthoringSafe = true;
	using Bounds = std::array<double, 3>;

	TerraEngine::UUID profileAssetId = TerraEngine::EMPTY_UUID;
	bool global = false;
	// Local-space AABB in world units. At the protected capture boundary, the
	// producer transforms all eight corners into the camera's same double frame.
	// The owner's UUID, rather than an editable field here, is the stable tie key.
	Bounds boundsMinimum{-0.5, -0.5, -0.5};
	Bounds boundsMaximum{0.5, 0.5, 0.5};
	std::uint64_t layerMask = ~std::uint64_t{0};
	std::int32_t priority = 0;
	float blendDistance = 0.0f;
	float weight = 1.0f;

	// Pure copied-value validation is usable in parallel preparation jobs. It
	// does not authorize reading live ECS storage outside its protected boundary.
	[[nodiscard]] TERRAENGINE_API bool ValidateAuthoredValues() const noexcept;
	TERRAENGINE_API static nlohmann::json SerializeAuthored(const PostProcessVolumeComponent& value);
	TERRAENGINE_API static bool DeserializeAuthored(PostProcessVolumeComponent& destination,
		const nlohmann::json& json);
	TERRAENGINE_API static void reflect(entt::meta_factory<PostProcessVolumeComponent>& factory);

	// These checked accessors are called on detached transaction candidates.
	// Rejected property edits throw before assignment; existing queue preparation
	// converts that exception into atomic rejection. Use a whole component replace
	// when both bounds must move beyond the old box in one authored operation.
	TERRAENGINE_API void SetBoundsMinimum(Bounds value);
	TERRAENGINE_API void SetBoundsMaximum(Bounds value);
	TERRAENGINE_API void SetBlendDistance(float value);
	TERRAENGINE_API void SetWeight(float value);
	[[nodiscard]] Bounds GetBoundsMinimum() const noexcept { return boundsMinimum; }
	[[nodiscard]] Bounds GetBoundsMaximum() const noexcept { return boundsMaximum; }
	[[nodiscard]] float GetBlendDistance() const noexcept { return blendDistance; }
	[[nodiscard]] float GetWeight() const noexcept { return weight; }
};

static_assert(std::is_copy_constructible_v<PostProcessVolumeComponent>);
static_assert(std::is_nothrow_move_constructible_v<PostProcessVolumeComponent>);
static_assert(std::is_nothrow_move_assignable_v<PostProcessVolumeComponent>);
static_assert(std::is_nothrow_destructible_v<PostProcessVolumeComponent>);
