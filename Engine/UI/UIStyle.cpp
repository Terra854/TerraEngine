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
std::string TrimCopy(std::string_view text)
{
	std::size_t start = 0;
	while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
	{
		++start;
	}

	std::size_t end = text.size();
	while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
	{
		--end;
	}

	return std::string(text.substr(start, end - start));
}

std::string ToLowerCopy(std::string_view text)
{
	std::string lowered(text);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	return lowered;
}

std::vector<std::string> SplitWhitespaceSeparated(std::string_view text)
{
	std::vector<std::string> tokens;
	std::stringstream stream{ TrimCopy(text) };
	std::string token;
	while (stream >> token)
	{
		tokens.push_back(token);
	}

	return tokens;
}

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

std::optional<int> HexDigit(char value)
{
	if (value >= '0' && value <= '9')
	{
		return value - '0';
	}
	if (value >= 'a' && value <= 'f')
	{
		return 10 + (value - 'a');
	}
	if (value >= 'A' && value <= 'F')
	{
		return 10 + (value - 'A');
	}
	return std::nullopt;
}

std::optional<float> ParseHexComponent(char high, char low)
{
	const std::optional<int> highNibble = HexDigit(high);
	const std::optional<int> lowNibble = HexDigit(low);
	if (!highNibble.has_value() || !lowNibble.has_value())
	{
		return std::nullopt;
	}

	return static_cast<float>((*highNibble << 4) | *lowNibble) / 255.0f;
}

float LerpFloat(float from, float to, float alpha)
{
	return from + ((to - from) * alpha);
}

glm::vec4 LerpColor(const glm::vec4& from, const glm::vec4& to, float alpha)
{
	return glm::vec4(
		LerpFloat(from.x, to.x, alpha),
		LerpFloat(from.y, to.y, alpha),
		LerpFloat(from.z, to.z, alpha),
		LerpFloat(from.w, to.w, alpha));
}
}

UIValue UIValue::Auto()
{
	return UIValue{};
}

UIValue UIValue::Points(float nextValue)
{
	return UIValue{ nextValue, Unit::Points };
}

UIValue UIValue::Percent(float nextValue)
{
	return UIValue{ nextValue, Unit::Percent };
}

bool UIValue::IsAuto() const
{
	return unit == Unit::Auto;
}

UIEdgeValues UIEdgeValues::All(const UIValue& value)
{
	return UIEdgeValues{ value, value, value, value };
}

void UIStyle::MergeFrom(const UIStyle& source)
{
	const auto mergeValue = [](auto& destination, const auto& incoming) {
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

UIStyle UIStyle::Lerp(const UIStyle& from, const UIStyle& to, float alpha)
{
	UIStyle result = from;
	const float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

	if (from.backgroundColor.has_value() && to.backgroundColor.has_value())
	{
		result.backgroundColor = LerpColor(*from.backgroundColor, *to.backgroundColor, clampedAlpha);
	}
	else if (to.backgroundColor.has_value())
	{
		result.backgroundColor = to.backgroundColor;
	}

	if (from.color.has_value() && to.color.has_value())
	{
		result.color = LerpColor(*from.color, *to.color, clampedAlpha);
	}
	else if (to.color.has_value())
	{
		result.color = to.color;
	}

	if (from.opacity.has_value() && to.opacity.has_value())
	{
		result.opacity = LerpFloat(*from.opacity, *to.opacity, clampedAlpha);
	}
	else if (to.opacity.has_value())
	{
		result.opacity = to.opacity;
	}

	if (from.transform.scale.has_value() && to.transform.scale.has_value())
	{
		result.transform.scale = LerpFloat(*from.transform.scale, *to.transform.scale, clampedAlpha);
	}
	else if (to.transform.scale.has_value())
	{
		result.transform.scale = to.transform.scale;
	}

	if (from.transform.rotationDegrees.has_value() && to.transform.rotationDegrees.has_value())
	{
		result.transform.rotationDegrees = LerpFloat(*from.transform.rotationDegrees, *to.transform.rotationDegrees, clampedAlpha);
	}
	else if (to.transform.rotationDegrees.has_value())
	{
		result.transform.rotationDegrees = to.transform.rotationDegrees;
	}

	if (from.transform.translate.has_value() && to.transform.translate.has_value())
	{
		result.transform.translate = glm::vec2(
			LerpFloat(from.transform.translate->x, to.transform.translate->x, clampedAlpha),
			LerpFloat(from.transform.translate->y, to.transform.translate->y, clampedAlpha));
	}
	else if (to.transform.translate.has_value())
	{
		result.transform.translate = to.transform.translate;
	}

	return result;
}

bool UIStyle::HasRenderableBackground() const
{
	return backgroundColor.has_value() && backgroundColor->w > 0.001f;
}

std::optional<glm::vec4> ParseUIColor(std::string_view text)
{
	const std::string trimmed = TrimCopy(text);
	if (trimmed.empty())
	{
		return std::nullopt;
	}

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

		if (!red.has_value() || !green.has_value() || !blue.has_value() || !alpha.has_value())
		{
			return std::nullopt;
		}

		return glm::vec4(*red, *green, *blue, *alpha);
	}

	const std::string lowered = ToLowerCopy(trimmed);
	if (lowered.rfind("rgba(", 0) == 0 && lowered.back() == ')')
	{
		const std::string inner = lowered.substr(5, lowered.size() - 6);
		std::stringstream stream{ inner };
		std::string token;
		std::vector<float> values;
		while (std::getline(stream, token, ','))
		{
			const std::optional<float> parsed = ParseFloatToken(token);
			if (!parsed.has_value())
			{
				return std::nullopt;
			}
			values.push_back(*parsed);
		}

		if (values.size() != 4)
		{
			return std::nullopt;
		}

		return glm::vec4(values[0] / 255.0f, values[1] / 255.0f, values[2] / 255.0f, values[3]);
	}

	return std::nullopt;
}

std::optional<UIValue> ParseUIValue(std::string_view text)
{
	const std::string trimmed = ToLowerCopy(TrimCopy(text));
	if (trimmed.empty())
	{
		return std::nullopt;
	}

	if (trimmed == "auto")
	{
		return UIValue::Auto();
	}

	if (trimmed.ends_with('%'))
	{
		const std::optional<float> parsed = ParseFloatToken(std::string_view(trimmed.data(), trimmed.size() - 1));
		return parsed.has_value() ? std::optional<UIValue>(UIValue::Percent(*parsed)) : std::nullopt;
	}

	if (trimmed.ends_with("px"))
	{
		const std::optional<float> parsed = ParseFloatToken(std::string_view(trimmed.data(), trimmed.size() - 2));
		return parsed.has_value() ? std::optional<UIValue>(UIValue::Points(*parsed)) : std::nullopt;
	}

	const std::optional<float> parsed = ParseFloatToken(trimmed);
	return parsed.has_value() ? std::optional<UIValue>(UIValue::Points(*parsed)) : std::nullopt;
}

std::optional<glm::vec2> ParseUIVector2(std::string_view text)
{
	const std::vector<std::string> tokens = SplitWhitespaceSeparated(text);
	if (tokens.size() != 2)
	{
		return std::nullopt;
	}

	const std::optional<float> x = ParseFloatToken(tokens[0]);
	const std::optional<float> y = ParseFloatToken(tokens[1]);
	if (!x.has_value() || !y.has_value())
	{
		return std::nullopt;
	}

	return glm::vec2(*x, *y);
}

std::optional<UIEdgeValues> ParseUIEdgeValues(std::string_view text)
{
	const std::vector<std::string> tokens = SplitWhitespaceSeparated(text);
	if (tokens.empty() || tokens.size() > 4)
	{
		return std::nullopt;
	}

	std::vector<UIValue> values;
	values.reserve(tokens.size());
	for (const std::string& token : tokens)
	{
		const std::optional<UIValue> parsed = ParseUIValue(token);
		if (!parsed.has_value())
		{
			return std::nullopt;
		}
		values.push_back(*parsed);
	}

	if (values.size() == 1)
	{
		return UIEdgeValues::All(values[0]);
	}

	if (values.size() == 2)
	{
		return UIEdgeValues{ values[0], values[1], values[0], values[1] };
	}

	if (values.size() == 3)
	{
		return UIEdgeValues{ values[0], values[1], values[2], values[1] };
	}

	return UIEdgeValues{ values[0], values[1], values[2], values[3] };
}

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

UIElementType UIElementTypeFromName(std::string_view name)
{
	const std::string lowered = ToLowerCopy(name);
	if (lowered == "uiroot")
	{
		return UIElementType::Root;
	}
	if (lowered == "label")
	{
		return UIElementType::Label;
	}
	if (lowered == "button")
	{
		return UIElementType::Button;
	}
	if (lowered == "image")
	{
		return UIElementType::Image;
	}
	return UIElementType::VisualElement;
}
