// SpatialBounds.h — Spatial Bounding Volume Definitions
//
// Defines the foundational bounding volume types, such as AABBs and Spheres, used by the spatial system.
// It provides utility functions for volume transformation, intersection testing, and merging.
// These volumes represent the spatial extent of scene entities and are the basis for all visibility and collision tests.

#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace TerraEngine::Scene
{
	// SpatialBoundsState names the supported states explicitly so serialization and switches stay readable.
	enum class SpatialBoundsState : std::uint8_t
	{
		Empty,
		Valid,
		Fallback,
		Invalid
	};

	// SpatialBoundsSource names the supported states explicitly so serialization and switches stay readable.
	enum class SpatialBoundsSource : std::uint8_t
	{
		Empty,
		Mesh,
		Authored,
		Aggregate,
		Fallback
	};

	// SpatialBoundsChangeKind describes one exact protected-boundary operation.
	// Classification changes use Update and are applied as an atomic remove from
	// the old partition plus add to the new partition.
	enum class SpatialBoundsChangeKind : std::uint8_t
	{
		Add,
		Update,
		Remove
	};

	// struct SpatialAABB groups related state and operations so callers have one documented contract.
	struct SpatialAABB
	{
		glm::vec3 min = glm::vec3(0.0f);
		glm::vec3 max = glm::vec3(0.0f);
		SpatialBoundsState state = SpatialBoundsState::Empty;
		SpatialBoundsSource source = SpatialBoundsSource::Empty;

		[[nodiscard]] glm::vec3 Center() const
		{
			return (min + max) * 0.5f;
		}

		[[nodiscard]] glm::vec3 Extent() const
		{
			return (max - min) * 0.5f;
		}

		[[nodiscard]] bool IsUsable() const
		{
			return state == SpatialBoundsState::Valid || state == SpatialBoundsState::Fallback;
		}
	};

	// struct SpatialSphere groups related state and operations so callers have one documented contract.
	struct SpatialSphere
	{
		glm::vec3 center = glm::vec3(0.0f);
		float radius = 0.0f;
	};

	// struct SpatialRay groups related state and operations so callers have one documented contract.
	struct SpatialRay
	{
		glm::vec3 origin = glm::vec3(0.0f);
		glm::vec3 direction = glm::vec3(0.0f, 0.0f, -1.0f);
		float maxDistance = std::numeric_limits<float>::max();
	};

	// struct SpatialFrustum groups related state and operations so callers have one documented contract.
	struct SpatialFrustum
	{
		std::array<glm::vec4, 6> planes{};
	};

	[[nodiscard]] inline bool IsFinite(float value)
	{
		return std::isfinite(value) != 0;
	}

	[[nodiscard]] inline bool IsFinite(const glm::vec3& value)
	{
		return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
	}

	[[nodiscard]] inline SpatialAABB MakeInvalidBounds(SpatialBoundsSource source = SpatialBoundsSource::Empty)
	{
		SpatialAABB result{};
		result.state = SpatialBoundsState::Invalid;
		result.source = source;
		return result;
	}

	[[nodiscard]] inline SpatialAABB MakeAABB(const glm::vec3& minValue, const glm::vec3& maxValue, SpatialBoundsSource source)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsFinite(minValue) || !IsFinite(maxValue))
		{
			return MakeInvalidBounds(source);
		}

		SpatialAABB result{};
		result.min = glm::min(minValue, maxValue);
		result.max = glm::max(minValue, maxValue);
		result.state = SpatialBoundsState::Valid;
		result.source = source;
		return result;
	}

	[[nodiscard]] inline SpatialAABB MakeFallbackBounds(const glm::vec3& center, float radius)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsFinite(center) || !IsFinite(radius) || radius < 0.0f)
		{
			return MakeInvalidBounds(SpatialBoundsSource::Fallback);
		}

		const glm::vec3 extent(radius);
		SpatialAABB result = MakeAABB(center - extent, center + extent, SpatialBoundsSource::Fallback);
		result.state = SpatialBoundsState::Fallback;
		return result;
	}

	// Expand is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline void Expand(SpatialAABB& bounds, float epsilon)
	{
		if (!bounds.IsUsable() || !IsFinite(epsilon) || epsilon <= 0.0f)
		{
			return;
		}

		const glm::vec3 pad(epsilon);
		bounds.min -= pad;
		bounds.max += pad;
	}

	[[nodiscard]] inline SpatialAABB Merge(const SpatialAABB& left, const SpatialAABB& right)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!left.IsUsable())
		{
			return right;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!right.IsUsable())
		{
			return left;
		}

		SpatialAABB result = MakeAABB(glm::min(left.min, right.min), glm::max(left.max, right.max), left.source);
		result.state = (left.state == SpatialBoundsState::Fallback || right.state == SpatialBoundsState::Fallback)
			? SpatialBoundsState::Fallback
			: SpatialBoundsState::Valid;
		return result;
	}

	[[nodiscard]] inline SpatialAABB TransformAABB(const SpatialAABB& localBounds, const glm::mat4& transform, float epsilon = 0.001f)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!localBounds.IsUsable())
		{
			return localBounds;
		}

		const std::array<glm::vec3, 8> corners = {
			glm::vec3(localBounds.min.x, localBounds.min.y, localBounds.min.z),
			glm::vec3(localBounds.max.x, localBounds.min.y, localBounds.min.z),
			glm::vec3(localBounds.min.x, localBounds.max.y, localBounds.min.z),
			glm::vec3(localBounds.max.x, localBounds.max.y, localBounds.min.z),
			glm::vec3(localBounds.min.x, localBounds.min.y, localBounds.max.z),
			glm::vec3(localBounds.max.x, localBounds.min.y, localBounds.max.z),
			glm::vec3(localBounds.min.x, localBounds.max.y, localBounds.max.z),
			glm::vec3(localBounds.max.x, localBounds.max.y, localBounds.max.z)
		};

		glm::vec3 worldMin(std::numeric_limits<float>::max());
		glm::vec3 worldMax(std::numeric_limits<float>::lowest());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const glm::vec3& corner : corners)
		{
			const glm::vec4 transformed = transform * glm::vec4(corner, 1.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!IsFinite(glm::vec3(transformed)))
			{
				return MakeInvalidBounds(localBounds.source);
			}
			worldMin = glm::min(worldMin, glm::vec3(transformed));
			worldMax = glm::max(worldMax, glm::vec3(transformed));
		}

		SpatialAABB result = MakeAABB(worldMin, worldMax, localBounds.source);
		result.state = localBounds.state;
		// Expand transformed bounds slightly so objects resting exactly on a
		// partition or frustum plane do not flicker due to float roundoff.
		Expand(result, epsilon);
		return result;
	}

	[[nodiscard]] inline bool Contains(const SpatialAABB& outer, const SpatialAABB& inner)
	{
		return outer.IsUsable()
			&& inner.IsUsable()
			&& glm::all(glm::lessThanEqual(outer.min, inner.min))
			&& glm::all(glm::greaterThanEqual(outer.max, inner.max));
	}

	[[nodiscard]] inline bool Overlaps(const SpatialAABB& left, const SpatialAABB& right)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!left.IsUsable() || !right.IsUsable())
		{
			return false;
		}

		return left.min.x <= right.max.x && left.max.x >= right.min.x
			&& left.min.y <= right.max.y && left.max.y >= right.min.y
			&& left.min.z <= right.max.z && left.max.z >= right.min.z;
	}

	[[nodiscard]] inline bool Intersects(const SpatialSphere& sphere, const SpatialAABB& bounds)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!bounds.IsUsable() || !IsFinite(sphere.center) || !IsFinite(sphere.radius) || sphere.radius < 0.0f)
		{
			return false;
		}

		const glm::vec3 closest = glm::clamp(sphere.center, bounds.min, bounds.max);
		const glm::vec3 delta = closest - sphere.center;
		return glm::dot(delta, delta) <= sphere.radius * sphere.radius;
	}

	[[nodiscard]] inline bool Intersects(const SpatialFrustum& frustum, const SpatialAABB& bounds)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!bounds.IsUsable())
		{
			return false;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const glm::vec4& plane : frustum.planes)
		{
			const glm::vec3 positive(
				plane.x >= 0.0f ? bounds.max.x : bounds.min.x,
				plane.y >= 0.0f ? bounds.max.y : bounds.min.y,
				plane.z >= 0.0f ? bounds.max.z : bounds.min.z);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline std::optional<float> Intersects(const SpatialRay& ray, const SpatialAABB& bounds)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!bounds.IsUsable() || !IsFinite(ray.origin) || !IsFinite(ray.direction) || !IsFinite(ray.maxDistance) || ray.maxDistance < 0.0f)
		{
			return std::nullopt;
		}

		const float lengthSq = glm::dot(ray.direction, ray.direction);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lengthSq <= 1e-10f)
		{
			return std::nullopt;
		}

		const glm::vec3 dir = ray.direction / std::sqrt(lengthSq);
		float tMin = 0.0f;
		float tMax = ray.maxDistance;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int axis = 0; axis < 3; ++axis)
		{
			if (std::abs(dir[axis]) < 1e-8f)
			{
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ray.origin[axis] < bounds.min[axis] || ray.origin[axis] > bounds.max[axis])
				{
					return std::nullopt;
				}
				continue;
			}

			const float invD = 1.0f / dir[axis];
			float t0 = (bounds.min[axis] - ray.origin[axis]) * invD;
			float t1 = (bounds.max[axis] - ray.origin[axis]) * invD;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (t0 > t1)
			{
				std::swap(t0, t1);
			}
			tMin = std::max(tMin, t0);
			tMax = std::min(tMax, t1);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (tMax < tMin)
			{
				return std::nullopt;
			}
		}
		return tMin;
	}

	[[nodiscard]] inline glm::vec4 NormalizePlane(const glm::vec4& plane)
	{
		const float length = glm::length(glm::vec3(plane));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (length <= 1e-8f || !IsFinite(length))
		{
			return plane;
		}
		return plane / length;
	}

	[[nodiscard]] inline SpatialFrustum ExtractFrustum(const glm::mat4& viewProjection)
	{
		auto row = [&](int index) {
			return glm::vec4(viewProjection[0][index], viewProjection[1][index], viewProjection[2][index], viewProjection[3][index]);
		};

		const glm::vec4 row0 = row(0);
		const glm::vec4 row1 = row(1);
		const glm::vec4 row2 = row(2);
		const glm::vec4 row3 = row(3);

		SpatialFrustum result{};
		result.planes[0] = NormalizePlane(row3 + row0);
		result.planes[1] = NormalizePlane(row3 - row0);
		result.planes[2] = NormalizePlane(row3 + row1);
		result.planes[3] = NormalizePlane(row3 - row1);
		result.planes[4] = NormalizePlane(row2);
		result.planes[5] = NormalizePlane(row3 - row2);
		return result;
	}
}
