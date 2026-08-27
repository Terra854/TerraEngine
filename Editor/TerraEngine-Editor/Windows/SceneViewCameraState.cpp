// SceneViewCameraState.cpp — Durable Scene View camera workspace state

#include "SceneViewCameraState.h"

#include "Engine/Scene/Serializer.h"

#include <cmath>
#include <string_view>

namespace TerraEngine::Editor::SceneView
{
	nlohmann::json SerializeCameraWorkspaceState(const CameraWorkspaceState& state)
	{
		return {
			{"schemaVersion", 1U},
			{"position", Serializer::SerializeWorldPosition(state.position)},
			{"rotationRadians", {
				state.rotationRadians.x, state.rotationRadians.y, state.rotationRadians.z}},
			{"focus", Serializer::SerializeWorldPosition(state.focus)},
			{"focusDistance", state.focusDistance},
			{"focusInitialized", state.focusInitialized},
			{"fieldOfViewDegrees", state.fieldOfViewDegrees},
			{"orthographicHeight", state.orthographicHeight},
			{"projection", state.projectionMode == SceneViewOverlay::ProjectionMode::Perspective
				? "perspective" : "orthographic"}
		};
	}

	std::optional<CameraWorkspaceState> DeserializeCameraWorkspaceState(
		const nlohmann::json& value,
		std::string* outError)
	{
		const auto fail = [outError](std::string message) -> std::optional<CameraWorkspaceState>
		{
			if(outError != nullptr)
			{
				*outError = std::move(message);
			}
			return std::nullopt;
		};
		if(!value.is_object() || !value.contains("schemaVersion")
			|| !value["schemaVersion"].is_number_unsigned()
			|| value["schemaVersion"].get<std::uint32_t>() != 1U)
		{
			return fail("Scene View camera state must use schemaVersion 1.");
		}

		std::string positionError;
		const auto position = value.contains("position")
			? Serializer::DeserializeWorldPosition(value["position"], &positionError) : std::nullopt;
		if(!position)
		{
			return fail("Scene View camera position is invalid: " + positionError);
		}
		std::string focusError;
		const auto focus = value.contains("focus")
			? Serializer::DeserializeWorldPosition(value["focus"], &focusError) : std::nullopt;
		if(!focus)
		{
			return fail("Scene View camera focus is invalid: " + focusError);
		}

		if(!value.contains("rotationRadians") || !value["rotationRadians"].is_array()
			|| value["rotationRadians"].size() != 3U)
		{
			return fail("Scene View camera rotationRadians must contain three finite numbers.");
		}
		glm::vec3 rotation{};
		for(std::size_t index = 0U; index < 3U; ++index)
		{
			if(!value["rotationRadians"][index].is_number())
			{
				return fail("Scene View camera rotationRadians must contain three finite numbers.");
			}
			const float component = value["rotationRadians"][index].get<float>();
			if(!std::isfinite(component))
			{
				return fail("Scene View camera rotationRadians must contain three finite numbers.");
			}
			rotation[static_cast<glm::length_t>(index)] = component;
		}

		const auto readBoundedFloat = [&value, &fail](
			std::string_view name,
			float minimum,
			float maximum,
			float& output) -> bool
		{
			const std::string key(name);
			if(!value.contains(key) || !value[key].is_number())
			{
				(void)fail("Scene View camera " + key + " must be a finite number.");
				return false;
			}
			const float decoded = value[key].get<float>();
			if(!std::isfinite(decoded) || decoded < minimum || decoded > maximum)
			{
				(void)fail("Scene View camera " + key + " is outside its supported range.");
				return false;
			}
			output = decoded;
			return true;
		};

		CameraWorkspaceState result;
		result.position = *position;
		result.focus = *focus;
		result.rotationRadians = rotation;
		if(!readBoundedFloat("focusDistance", 1.0F, 1000.0F, result.focusDistance)
			|| !readBoundedFloat("fieldOfViewDegrees", 1.0F, 179.0F, result.fieldOfViewDegrees)
			|| !readBoundedFloat("orthographicHeight", 1.0F, 500.0F, result.orthographicHeight))
		{
			return std::nullopt;
		}
		if(!value.contains("focusInitialized") || !value["focusInitialized"].is_boolean())
		{
			return fail("Scene View camera focusInitialized must be a boolean.");
		}
		result.focusInitialized = value["focusInitialized"].get<bool>();

		if(!value.contains("projection") || !value["projection"].is_string())
		{
			return fail("Scene View camera projection must be perspective or orthographic.");
		}
		const std::string projection = value["projection"].get<std::string>();
		if(projection == "perspective")
		{
			result.projectionMode = SceneViewOverlay::ProjectionMode::Perspective;
		}
		else if(projection == "orthographic")
		{
			result.projectionMode = SceneViewOverlay::ProjectionMode::Orthographic;
		}
		else
		{
			return fail("Scene View camera projection must be perspective or orthographic.");
		}
		if(outError != nullptr)
		{
			outError->clear();
		}
		return result;
	}
}
