// Transform.h — Component defining an object's canonical world and bounded local pose.
//
// As a fundamental component for almost all scene objects, it provides TRS
// (Translation, Rotation, Scale) data. Absolute translation is represented by
// an exact cell plus a bounded double offset; callers must not add a global
// float Position alias because that would silently lose distant-world accuracy.

#pragma once

#include "Component.h"
#include "Scene/WorldCoordinate.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>

namespace TerraEngine::Scene {
	inline bool IsWorldPoseFinite(const WorldPose& pose)
	{
		return std::isfinite(pose.rotation.w) && std::isfinite(pose.rotation.x)
			&& std::isfinite(pose.rotation.y) && std::isfinite(pose.rotation.z)
			&& std::isfinite(pose.scale.x) && std::isfinite(pose.scale.y) && std::isfinite(pose.scale.z);
	}

	// A hierarchy link uses a bounded local space so rotation and scale never
	// operate on an unbounded canonical address. The owner supplies the anchor.
	struct ParentRelativePose {
		glm::dvec3 translationMetres{0.0};
		glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 scale{1.0f, 1.0f, 1.0f};
		std::uint64_t transformSpaceId = 0;
	};

	inline bool IsParentRelativePoseInRange(const ParentRelativePose& pose)
	{
		const bool finiteTranslation = std::isfinite(pose.translationMetres.x)
			&& std::isfinite(pose.translationMetres.y)
			&& std::isfinite(pose.translationMetres.z);
		const bool finiteRotation = std::isfinite(pose.rotation.w)
			&& std::isfinite(pose.rotation.x)
			&& std::isfinite(pose.rotation.y)
			&& std::isfinite(pose.rotation.z);
		const bool finiteScale = std::isfinite(pose.scale.x)
			&& std::isfinite(pose.scale.y)
			&& std::isfinite(pose.scale.z);
		return finiteTranslation && finiteRotation && finiteScale
			&& glm::length(pose.translationMetres) <= kTransformSpaceMaximumRelativeRadiusMetres;
	}
}

// Basic spatial component that exposes TRS data via glm math types.
class Transform : public Component<Transform> {
public:
	static constexpr std::string_view kComponentName = "Transform";

	Transform()
	{
	}

	// worldPose is the single absolute spatial authority for this object.
	TerraEngine::Scene::WorldPose worldPose{};
	// parentRelativePose is only meaningful inside the hierarchy's bounded
	// transform space and cannot replace the canonical world pose.
	TerraEngine::Scene::ParentRelativePose parentRelativePose{};


	static void reflect(entt::meta_factory<Transform>& factory)
	{
		// The reflected values are intentionally structured. Tooling must issue
		// typed world-position operations instead of setting a lossy float axis.
		TerraEngine::Reflection::RegisterDataProperty<&Transform::worldPose>(factory, "WorldPose");
		TerraEngine::Reflection::RegisterDataProperty<&Transform::parentRelativePose>(factory, "ParentRelativePose");
	}
};
