#include "Scene/PostProcessVolumeComponent.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
	bool ValidBounds(const PostProcessVolumeComponent::Bounds& minimum,
		const PostProcessVolumeComponent::Bounds& maximum) noexcept
	{
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis])
				|| minimum[axis] > maximum[axis]) return false;
		}
		return true;
	}
}

bool PostProcessVolumeComponent::ValidateAuthoredValues() const noexcept
{
	return ValidBounds(boundsMinimum, boundsMaximum)
		&& std::isfinite(blendDistance) && blendDistance >= 0.0f
		&& std::isfinite(weight) && weight >= 0.0f && weight <= 1.0f;
}

nlohmann::json PostProcessVolumeComponent::SerializeAuthored(const PostProcessVolumeComponent& value)
{
	// Keep uint64 masks and double bounds exact; the generic scalar-only component
	// JSON bridge cannot represent this complete authored contract on its own.
	return {{"active", value.active}, {"profileAssetId", TerraEngine::ToHexString(value.profileAssetId)},
		{"global", value.global}, {"boundsMinimum", value.boundsMinimum}, {"boundsMaximum", value.boundsMaximum},
		{"layerMask", value.layerMask}, {"priority", value.priority},
		{"blendDistance", value.blendDistance}, {"weight", value.weight}};
}

bool PostProcessVolumeComponent::DeserializeAuthored(PostProcessVolumeComponent& destination,
	const nlohmann::json& json)
{
	if (!json.is_object()) return false;
	// Decode every field into an isolated candidate. Even a late invalid value or
	// unknown property leaves the supplied destination entirely unchanged.
	PostProcessVolumeComponent candidate;
	for (const auto& [name, value] : json.items())
	{
		if (name == "active" || name == "global")
		{
			if (!value.is_boolean()) return false;
			(name == "active" ? candidate.active : candidate.global) = value.get<bool>();
		}
		else if (name == "profileAssetId")
		{
			if (!value.is_string()) return false;
			const auto id = TerraEngine::ParseUUID(value.get<std::string>());
			if (!id) return false;
			candidate.profileAssetId = *id;
		}
		else if (name == "boundsMinimum" || name == "boundsMaximum")
		{
			if (!value.is_array() || value.size() != 3) return false;
			auto& bounds = name == "boundsMinimum" ? candidate.boundsMinimum : candidate.boundsMaximum;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (!value[axis].is_number()) return false;
				bounds[axis] = value[axis].get<double>();
			}
		}
		else if (name == "layerMask")
		{
			// Never pass uint64 masks through double: upper bits must survive scene
			// save/load and negative signed JSON must not wrap into all layers.
			if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) return false;
			candidate.layerMask = value.get<std::uint64_t>();
		}
		else if (name == "priority")
		{
			if (!value.is_number_integer()) return false;
			if (value.is_number_unsigned())
			{
				if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) return false;
			}
			else if (value.get<std::int64_t>() < std::numeric_limits<std::int32_t>::min()
				|| value.get<std::int64_t>() > std::numeric_limits<std::int32_t>::max()) return false;
			candidate.priority = value.get<std::int32_t>();
		}
		else if (name == "blendDistance" || name == "weight")
		{
			if (!value.is_number()) return false;
			const double scalar = value.get<double>();
			if (!std::isfinite(scalar) || scalar < 0.0 || scalar > std::numeric_limits<float>::max()
				|| (name == "weight" && scalar > 1.0)) return false;
			(name == "blendDistance" ? candidate.blendDistance : candidate.weight) = static_cast<float>(scalar);
		}
		else return false;
	}
	if (!candidate.ValidateAuthoredValues()) return false;
	destination = std::move(candidate);
	return true;
}

void PostProcessVolumeComponent::SetBoundsMinimum(Bounds value)
{
	if (!ValidBounds(value, boundsMaximum)) throw std::invalid_argument("Post-process volume minimum must be finite and not exceed its maximum.");
	boundsMinimum = value;
}
void PostProcessVolumeComponent::SetBoundsMaximum(Bounds value)
{
	if (!ValidBounds(boundsMinimum, value)) throw std::invalid_argument("Post-process volume maximum must be finite and not precede its minimum.");
	boundsMaximum = value;
}
void PostProcessVolumeComponent::SetBlendDistance(float value)
{
	if (!std::isfinite(value) || value < 0.0f) throw std::invalid_argument("Post-process volume blend distance must be finite and nonnegative.");
	blendDistance = value;
}
void PostProcessVolumeComponent::SetWeight(float value)
{
	if (!std::isfinite(value) || value < 0.0f || value > 1.0f) throw std::invalid_argument("Post-process volume weight must be finite and in0..1.");
	weight = value;
}

void PostProcessVolumeComponent::reflect(entt::meta_factory<PostProcessVolumeComponent>& factory)
{
	TerraEngine::Reflection::RegisterDataProperty<&PostProcessVolumeComponent::profileAssetId>(factory, "profileAssetId");
	TerraEngine::Reflection::RegisterDataProperty<&PostProcessVolumeComponent::global>(factory, "global");
	TerraEngine::Reflection::RegisterAccessorProperty<&PostProcessVolumeComponent::SetBoundsMinimum, &PostProcessVolumeComponent::GetBoundsMinimum>(factory, "boundsMinimum");
	TerraEngine::Reflection::RegisterAccessorProperty<&PostProcessVolumeComponent::SetBoundsMaximum, &PostProcessVolumeComponent::GetBoundsMaximum>(factory, "boundsMaximum");
	TerraEngine::Reflection::RegisterDataProperty<&PostProcessVolumeComponent::layerMask>(factory, "layerMask");
	TerraEngine::Reflection::RegisterDataProperty<&PostProcessVolumeComponent::priority>(factory, "priority");
	TerraEngine::Reflection::RegisterAccessorProperty<&PostProcessVolumeComponent::SetBlendDistance, &PostProcessVolumeComponent::GetBlendDistance>(factory, "blendDistance");
	TerraEngine::Reflection::RegisterAccessorProperty<&PostProcessVolumeComponent::SetWeight, &PostProcessVolumeComponent::GetWeight>(factory, "weight");
}
