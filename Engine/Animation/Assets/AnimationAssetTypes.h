// AnimationAssetTypes.h - Fixed-width math records shared by cooked animation assets.
//
// These small value types deliberately avoid pointers and allocator-owned state.
// Concrete asset serializers therefore have one explicit, testable conversion
// boundary instead of copying compiler- or platform-specific math layouts to disk.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace TerraEngine::Animation {
	struct AnimationFloat3 {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;

		auto operator<=>(const AnimationFloat3&) const = default;
	};

	struct AnimationQuaternion {
		float x = 0.0F;
		float y = 0.0F;
		float z = 0.0F;
		float w = 1.0F;

		auto operator<=>(const AnimationQuaternion&) const = default;
	};

	struct AnimationTransform {
		AnimationFloat3 translation{};
		AnimationQuaternion rotation{};
		AnimationFloat3 scale{1.0F, 1.0F, 1.0F};

		auto operator<=>(const AnimationTransform&) const = default;
	};

	struct AnimationMatrix4x4 {
		std::array<float, 16U> values{};

		auto operator<=>(const AnimationMatrix4x4&) const = default;
	};

	inline bool IsFinite(const AnimationFloat3& value) noexcept {
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	inline bool IsFinite(const AnimationQuaternion& value) noexcept {
		return std::isfinite(value.x) && std::isfinite(value.y)
			&& std::isfinite(value.z) && std::isfinite(value.w);
	}

	inline bool IsFinite(const AnimationTransform& value) noexcept {
		return IsFinite(value.translation) && IsFinite(value.rotation) && IsFinite(value.scale);
	}

	inline bool IsFinite(const AnimationMatrix4x4& value) noexcept {
		for(const float component : value.values) {
			if(!std::isfinite(component)) {
				return false;
			}
		}
		return true;
	}

	inline bool IsNormalized(const AnimationQuaternion& value, float tolerance = 0.001F) noexcept {
		if(!IsFinite(value)) {
			return false;
		}
		const float lengthSquared = value.x * value.x + value.y * value.y
			+ value.z * value.z + value.w * value.w;
		return std::abs(lengthSquared - 1.0F) <= tolerance;
	}
}
