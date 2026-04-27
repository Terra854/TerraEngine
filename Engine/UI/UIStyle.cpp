// UIStyle.cpp — Implementation of UI style merging and property parsing.
//
// This file implements the logic for merging multiple style rules,
// interpolating between styles for animations, and parsing string-based
// representations of colors, units, and edge values into their corresponding
// engine types.

#include "UIStyle.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// TrimCopy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string TrimCopy(std::string_view text)
{
	std::size_t start = 0;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
	{
		++start;
	}

	std::size_t end = text.size();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
	{
		--end;
	}

	return std::string(text.substr(start, end - start));
}

// ToLowerCopy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ToLowerCopy(std::string_view text)
{
	std::string lowered(text);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	return lowered;
}

// SplitWhitespaceSeparated is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<std::string> SplitWhitespaceSeparated(std::string_view text)
{
	std::vector<std::string> tokens;
	std::stringstream stream{ TrimCopy(text) };
	std::string token;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (stream >> token)
	{
		tokens.push_back(token);
	}

	return tokens;
}

// ParseFloatToken is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<float> ParseFloatToken(std::string_view text)
{
	try
	{
		return std::stof(TrimCopy(text));
	}
	catch (...)
	{
		return std::nullopt;
	}
}

// HexDigit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<int> HexDigit(char value)
{
	if (value >= '0' && value <= '9')
	{
		return value - '0';
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (value >= 'a' && value <= 'f')
	{
		return 10 + (value - 'a');
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (value >= 'A' && value <= 'F')
	{
		return 10 + (value - 'A');
	}
	return std::nullopt;
}

// ParseHexComponent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<float> ParseHexComponent(char high, char low)
{
	const std::optional<int> highNibble = HexDigit(high);
	const std::optional<int> lowNibble = HexDigit(low);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!highNibble.has_value() || !lowNibble.has_value())
	{
		return std::nullopt;
	}

	return static_cast<float>((*highNibble << 4) | *lowNibble) / 255.0f;
}

// LerpFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
float LerpFloat(float from, float to, float alpha)
{
	return from + ((to - from) * alpha);
}

// LerpColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
glm::vec4 LerpColor(const glm::vec4& from, const glm::vec4& to, float alpha)
{
	return glm::vec4(
		LerpFloat(from.x, to.x, alpha),
		LerpFloat(from.y, to.y, alpha),
		LerpFloat(from.z, to.z, alpha),
		LerpFloat(from.w, to.w, alpha));
}
}

// UIValue::Auto is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIValue UIValue::Auto()
{
	return UIValue{};
}

// UIValue::Points is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIValue UIValue::Points(float nextValue)
{
	return UIValue{ nextValue, Unit::Points };
}

// UIValue::Percent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIValue UIValue::Percent(float nextValue)
{
	return UIValue{ nextValue, Unit::Percent };
}

// UIValue::IsAuto is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIValue::IsAuto() const
{
	return unit == Unit::Auto;
}

// UIEdgeValues::All is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIEdgeValues UIEdgeValues::All(const UIValue& value)
{
	return UIEdgeValues{ value, value, value, value };
}

// UIStyle::MergeFrom is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIStyle::MergeFrom(const UIStyle& source)
{
	const auto mergeValue = [](auto& destination, const auto& incoming) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (incoming.has_value())
		{
			destination = incoming;
		}
	};

	mergeValue(backgroundColor, source.backgroundColor);
	mergeValue(color, source.color);
	mergeValue(opacity, source.opacity);
	mergeValue(fontSize, source.fontSize);
	mergeValue(fontFamily, source.fontFamily);
	mergeValue(flexDirection, source.flexDirection);
	mergeValue(justifyContent, source.justifyContent);
	mergeValue(alignItems, source.alignItems);
	mergeValue(flexGrow, source.flexGrow);
	mergeValue(flexShrink, source.flexShrink);
	mergeValue(flexBasis, source.flexBasis);
	mergeValue(width, source.width);
	mergeValue(height, source.height);
	mergeValue(margin, source.margin);
	mergeValue(padding, source.padding);
	mergeValue(borderWidth, source.borderWidth);
	mergeValue(borderRadius, source.borderRadius);
	mergeValue(transform.translate, source.transform.translate);
	mergeValue(transform.scale, source.transform.scale);
	mergeValue(transform.rotationDegrees, source.transform.rotationDegrees);
}

// UIStyle::Lerp is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIStyle UIStyle::Lerp(const UIStyle& from, const UIStyle& to, float alpha)
{
	UIStyle result = from;
	const float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (from.backgroundColor.has_value() && to.backgroundColor.has_value())
	{
		result.backgroundColor = LerpColor(*from.backgroundColor, *to.backgroundColor, clampedAlpha);
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (to.backgroundColor.has_value())
	{
		result.backgroundColor = to.backgroundColor;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (from.color.has_value() && to.color.has_value())
	{
		result.color = LerpColor(*from.color, *to.color, clampedAlpha);
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (to.color.has_value())
	{
		result.color = to.color;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (from.opacity.has_value() && to.opacity.has_value())
	{
		result.opacity = LerpFloat(*from.opacity, *to.opacity, clampedAlpha);
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (to.opacity.has_value())
	{
		result.opacity = to.opacity;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (from.transform.scale.has_value() && to.transform.scale.has_value())
	{
		result.transform.scale = LerpFloat(*from.transform.scale, *to.transform.scale, clampedAlpha);
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (to.transform.scale.has_value())
	{
		result.transform.scale = to.transform.scale;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (from.transform.rotationDegrees.has_value() && to.transform.rotationDegrees.has_value())
	{
		result.transform.rotationDegrees = LerpFloat(*from.transform.rotationDegrees, *to.transform.rotationDegrees, clampedAlpha);
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (to.transform.rotationDegrees.has_value())
	{
		result.transform.rotationDegrees = to.transform.rotationDegrees;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (from.transform.translate.has_value() && to.transform.translate.has_value())
	{
		result.transform.translate = glm::vec2(
			LerpFloat(from.transform.translate->x, to.transform.translate->x, clampedAlpha),
			LerpFloat(from.transform.translate->y, to.transform.translate->y, clampedAlpha));
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (to.transform.translate.has_value())
	{
		result.transform.translate = to.transform.translate;
	}

	return result;
}

// UIStyle::HasRenderableBackground is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIStyle::HasRenderableBackground() const
{
	return backgroundColor.has_value() && backgroundColor->w > 0.001f;
}

// ParseUIColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<glm::vec4> ParseUIColor(std::string_view text)
{
	const std::string trimmed = TrimCopy(text);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmed.empty())
	{
		return std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmed[0] == '#')
	{
		if (trimmed.size() != 7 && trimmed.size() != 9)
		{
			return std::nullopt;
		}

		const std::optional<float> red = ParseHexComponent(trimmed[1], trimmed[2]);
		const std::optional<float> green = ParseHexComponent(trimmed[3], trimmed[4]);
		const std::optional<float> blue = ParseHexComponent(trimmed[5], trimmed[6]);
		const std::optional<float> alpha = trimmed.size() == 9
			? ParseHexComponent(trimmed[7], trimmed[8])
			: std::optional<float>(1.0f);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!red.has_value() || !green.has_value() || !blue.has_value() || !alpha.has_value())
		{
			return std::nullopt;
		}

		return glm::vec4(*red, *green, *blue, *alpha);
	}

	const std::string lowered = ToLowerCopy(trimmed);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lowered.rfind("rgba(", 0) == 0 && lowered.back() == ')')
	{
		const std::string inner = lowered.substr(5, lowered.size() - 6);
		std::stringstream stream{ inner };
		std::string token;
		std::vector<float> values;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (std::getline(stream, token, ','))
		{
			const std::optional<float> parsed = ParseFloatToken(token);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!parsed.has_value())
			{
				return std::nullopt;
			}
			values.push_back(*parsed);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (values.size() != 4)
		{
			return std::nullopt;
		}

		return glm::vec4(values[0] / 255.0f, values[1] / 255.0f, values[2] / 255.0f, values[3]);
	}

	return std::nullopt;
}

// ParseUIValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<UIValue> ParseUIValue(std::string_view text)
{
	const std::string trimmed = ToLowerCopy(TrimCopy(text));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmed.empty())
	{
		return std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmed == "auto")
	{
		return UIValue::Auto();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmed.ends_with('%'))
	{
		const std::optional<float> parsed = ParseFloatToken(std::string_view(trimmed.data(), trimmed.size() - 1));
		return parsed.has_value() ? std::optional<UIValue>(UIValue::Percent(*parsed)) : std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmed.ends_with("px"))
	{
		const std::optional<float> parsed = ParseFloatToken(std::string_view(trimmed.data(), trimmed.size() - 2));
		return parsed.has_value() ? std::optional<UIValue>(UIValue::Points(*parsed)) : std::nullopt;
	}

	const std::optional<float> parsed = ParseFloatToken(trimmed);
	return parsed.has_value() ? std::optional<UIValue>(UIValue::Points(*parsed)) : std::nullopt;
}

// ParseUIVector2 is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<glm::vec2> ParseUIVector2(std::string_view text)
{
	const std::vector<std::string> tokens = SplitWhitespaceSeparated(text);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (tokens.size() != 2)
	{
		return std::nullopt;
	}

	const std::optional<float> x = ParseFloatToken(tokens[0]);
	const std::optional<float> y = ParseFloatToken(tokens[1]);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!x.has_value() || !y.has_value())
	{
		return std::nullopt;
	}

	return glm::vec2(*x, *y);
}

// ParseUIEdgeValues is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<UIEdgeValues> ParseUIEdgeValues(std::string_view text)
{
	const std::vector<std::string> tokens = SplitWhitespaceSeparated(text);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (tokens.empty() || tokens.size() > 4)
	{
		return std::nullopt;
	}

	std::vector<UIValue> values;
	values.reserve(tokens.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::string& token : tokens)
	{
		const std::optional<UIValue> parsed = ParseUIValue(token);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!parsed.has_value())
		{
			return std::nullopt;
		}
		values.push_back(*parsed);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (values.size() == 1)
	{
		return UIEdgeValues::All(values[0]);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (values.size() == 2)
	{
		return UIEdgeValues{ values[0], values[1], values[0], values[1] };
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (values.size() == 3)
	{
		return UIEdgeValues{ values[0], values[1], values[2], values[1] };
	}

	return UIEdgeValues{ values[0], values[1], values[2], values[3] };
}

// ToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ToString(UIElementType type)
{
	switch (type)
	{
	case UIElementType::Root:
		return "UIRoot";
	case UIElementType::VisualElement:
		return "VisualElement";
	case UIElementType::Label:
		return "Label";
	case UIElementType::Button:
		return "Button";
	case UIElementType::Image:
		return "Image";
	default:
		return "VisualElement";
	}
}

// UIElementTypeFromName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElementType UIElementTypeFromName(std::string_view name)
{
	const std::string lowered = ToLowerCopy(name);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lowered == "uiroot")
	{
		return UIElementType::Root;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lowered == "label")
	{
		return UIElementType::Label;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lowered == "button")
	{
		return UIElementType::Button;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lowered == "image")
	{
		return UIElementType::Image;
	}
	return UIElementType::VisualElement;
}
