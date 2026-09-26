// CharacterLocomotionComponent.h - durable character movement authoring only.

#pragma once

#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// A scene stores these copied settings; the locomotion system owns every runtime
// token, lease, command and observation separately. Binding values are fixed-width
// integers so the existing reflection bridge can edit them transactionally.
struct CharacterLocomotionComponent final : public Component<CharacterLocomotionComponent> {
	static constexpr std::string_view kComponentName = "CharacterLocomotionComponent";
	static constexpr std::uint32_t kNoneBinding = 0U;
	static constexpr std::uint32_t kSelfBinding = 1U;
	static constexpr std::uint32_t kDescendantBinding = 2U;
	static constexpr std::uint32_t kFacingHold = 0U;
	static constexpr std::uint32_t kFacingMovement = 1U;
	static constexpr std::uint32_t kVelocityMode = 0U;
	static constexpr std::uint32_t kRootMotionMode = 1U;

	// Controller binding has only Self in version one. Navigation may be None or
	// Self; an animator may also name a same-scene descendant by UUID.
	std::uint32_t controllerBinding = kSelfBinding;
	std::uint32_t navigationBinding = kNoneBinding;
	std::uint32_t animatorBinding = kNoneBinding;
	TerraEngine::UUID animatorObjectId = TerraEngine::EMPTY_UUID;
	double maximumSpeed = 5.0; // metres/second, inclusive range [0, 100]
	double acceleration = 20.0; // metres/second squared, range (0, 1000]
	double braking = 25.0; // metres/second squared, range (0, 1000]
	double turnRate = 6.283185; // radians/second, inclusive range [0, 100]
	std::uint32_t facing = kFacingMovement;
	std::uint32_t initialMotionMode = kVelocityMode;
	std::string speedParameter;
	std::string localDirectionXParameter;
	std::string localDirectionZParameter;
	std::string groundedParameter;
	std::string movingParameter;

	// This checks only durable value shape. Existence, descendant hierarchy,
	// controller geometry and animator parameter types need live binding evidence.
	[[nodiscard]] bool ValidateAuthoredValues() const noexcept {
		if(controllerBinding != kSelfBinding || navigationBinding > kSelfBinding
			|| animatorBinding > kDescendantBinding || facing > kFacingMovement
			|| initialMotionMode > kRootMotionMode) return false;
		if((animatorBinding == kDescendantBinding) != (animatorObjectId != TerraEngine::EMPTY_UUID)) return false;
		if(!std::isfinite(maximumSpeed) || maximumSpeed < 0.0 || maximumSpeed > 100.0
			|| !std::isfinite(acceleration) || acceleration <= 0.0 || acceleration > 1000.0
			|| !std::isfinite(braking) || braking <= 0.0 || braking > 1000.0
			|| !std::isfinite(turnRate) || turnRate < 0.0 || turnRate > 100.0) return false;
		const std::array<std::string_view, 5U> names{speedParameter, localDirectionXParameter,
			localDirectionZParameter, groundedParameter, movingParameter};
		for(std::size_t index = 0U; index < names.size(); ++index) {
			if(!ValidParameterName(names[index])) return false;
			for(std::size_t other = 0U; other < index; ++other)
				if(!names[index].empty() && names[index] == names[other]) return false;
		}
		return true;
	}

	static nlohmann::json SerializeAuthored(const CharacterLocomotionComponent& value) {
		return {{"active", value.active}, {"controllerBinding", value.controllerBinding},
			{"navigationBinding", value.navigationBinding}, {"animatorBinding", value.animatorBinding},
			{"animatorObjectId", TerraEngine::ToHexString(value.animatorObjectId)},
			{"maximumSpeed", value.maximumSpeed}, {"acceleration", value.acceleration},
			{"braking", value.braking}, {"turnRate", value.turnRate}, {"facing", value.facing},
			{"initialMotionMode", value.initialMotionMode}, {"speedParameter", value.speedParameter},
			{"localDirectionXParameter", value.localDirectionXParameter},
			{"localDirectionZParameter", value.localDirectionZParameter},
			{"groundedParameter", value.groundedParameter}, {"movingParameter", value.movingParameter}};
	}

	// Missing values use the documented defaults. The destination receives the
	// complete candidate only after every field and cross-field rule succeeds.
	static bool DeserializeAuthored(CharacterLocomotionComponent& destination, const nlohmann::json& json) {
		if(!json.is_object()) return false;
		CharacterLocomotionComponent candidate{};
		auto readBool = [&json](std::string_view key, bool& output) {
			const auto it = json.find(std::string(key));
			if(it == json.end()) return true;
			if(!it->is_boolean()) return false;
			output = it->get<bool>();
			return true;
		};
		auto readInteger = [&json](std::string_view key, std::uint32_t& output) {
			const auto it = json.find(std::string(key));
			if(it == json.end()) return true;
			if(!it->is_number_integer()) return false;
			if(it->is_number_unsigned()) {
				const auto value = it->get<std::uint64_t>();
				if(value > std::numeric_limits<std::uint32_t>::max()) return false;
				output = static_cast<std::uint32_t>(value);
			} else {
				const auto value = it->get<std::int64_t>();
				if(value < 0 || value > std::numeric_limits<std::uint32_t>::max()) return false;
				output = static_cast<std::uint32_t>(value);
			}
			return true;
		};
		auto readDouble = [&json](std::string_view key, double& output) {
			const auto it = json.find(std::string(key));
			if(it == json.end()) return true;
			if(!it->is_number()) return false;
			output = it->get<double>();
			return std::isfinite(output);
		};
		auto readString = [&json](std::string_view key, std::string& output) {
			const auto it = json.find(std::string(key));
			if(it == json.end()) return true;
			if(!it->is_string()) return false;
			output = it->get<std::string>();
			return true;
		};
		if(!readBool("active", candidate.active)
			|| !readInteger("controllerBinding", candidate.controllerBinding)
			|| !readInteger("navigationBinding", candidate.navigationBinding)
			|| !readInteger("animatorBinding", candidate.animatorBinding)
			|| !readDouble("maximumSpeed", candidate.maximumSpeed)
			|| !readDouble("acceleration", candidate.acceleration)
			|| !readDouble("braking", candidate.braking)
			|| !readDouble("turnRate", candidate.turnRate)
			|| !readInteger("facing", candidate.facing)
			|| !readInteger("initialMotionMode", candidate.initialMotionMode)
			|| !readString("speedParameter", candidate.speedParameter)
			|| !readString("localDirectionXParameter", candidate.localDirectionXParameter)
			|| !readString("localDirectionZParameter", candidate.localDirectionZParameter)
			|| !readString("groundedParameter", candidate.groundedParameter)
			|| !readString("movingParameter", candidate.movingParameter)) return false;
		if(const auto it = json.find("animatorObjectId"); it != json.end()) {
			if(!it->is_string()) return false;
			const auto parsed = TerraEngine::ParseUUID(it->get<std::string>());
			if(!parsed) return false;
			candidate.animatorObjectId = *parsed;
		}
		if(!candidate.ValidateAuthoredValues()) return false;
		destination = candidate;
		return true;
	}

	static void reflect(entt::meta_factory<CharacterLocomotionComponent>& factory) {
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::controllerBinding>(factory, "controllerBinding");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::navigationBinding>(factory, "navigationBinding");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::animatorBinding>(factory, "animatorBinding");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::animatorObjectId>(factory, "animatorObjectId");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::maximumSpeed>(factory, "maximumSpeed");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::acceleration>(factory, "acceleration");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::braking>(factory, "braking");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::turnRate>(factory, "turnRate");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::facing>(factory, "facing");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::initialMotionMode>(factory, "initialMotionMode");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::speedParameter>(factory, "speedParameter");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::localDirectionXParameter>(factory, "localDirectionXParameter");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::localDirectionZParameter>(factory, "localDirectionZParameter");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::groundedParameter>(factory, "groundedParameter");
		TerraEngine::Reflection::RegisterDataProperty<&CharacterLocomotionComponent::movingParameter>(factory, "movingParameter");
	}

private:
	[[nodiscard]] static bool ValidParameterName(std::string_view text) noexcept {
		if(text.size() > 128U) return false;
		// Reject overlong encodings, surrogate code points and truncated sequences;
		// animation parameter lookup uses byte-exact UTF-8 names.
		for(std::size_t index = 0U; index < text.size();) {
			const auto lead = static_cast<unsigned char>(text[index]);
			if(lead == 0U) return false;
			if(lead < 0x80U) { ++index; continue; }
			std::size_t count = 0U;
			std::uint32_t code = 0U;
			if((lead & 0xE0U) == 0xC0U) { count = 2U; code = lead & 0x1FU; }
			else if((lead & 0xF0U) == 0xE0U) { count = 3U; code = lead & 0x0FU; }
			else if((lead & 0xF8U) == 0xF0U) { count = 4U; code = lead & 0x07U; }
			else return false;
			if(index + count > text.size()) return false;
			for(std::size_t offset = 1U; offset < count; ++offset) {
				const auto continuation = static_cast<unsigned char>(text[index + offset]);
				if((continuation & 0xC0U) != 0x80U) return false;
				code = (code << 6U) | (continuation & 0x3FU);
			}
			if((count == 2U && code < 0x80U) || (count == 3U && code < 0x800U)
				|| (count == 4U && code < 0x10000U) || code > 0x10FFFFU
				|| (code >= 0xD800U && code <= 0xDFFFU)) return false;
			index += count;
		}
		return true;
	}
};
