// UIParser.cpp — Implementation of the UI markup and style parser.
//
// This file implements the heavy lifting of parsing XML documents into
// UIElements and interpreting CSS-like style rules. It includes specialized
// logic for selector matching, property inheritance, and the conversion of
// string-based attributes into engine-native types like colors and enums.

#include "UIParser.h"

#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

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

// ParseStyleSourceList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<std::string> ParseStyleSourceList(std::string_view serializedPaths)
{
	std::vector<std::string> parsedPaths;
	std::size_t cursor = 0;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (cursor <= serializedPaths.size())
	{
		const std::size_t delimiter = serializedPaths.find(';', cursor);
		const std::string cleanedToken = TrimCopy(serializedPaths.substr(
			cursor,
			delimiter == std::string_view::npos ? serializedPaths.size() - cursor : delimiter - cursor));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!cleanedToken.empty())
		{
			parsedPaths.push_back(cleanedToken);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (delimiter == std::string_view::npos)
		{
			break;
		}

		cursor = delimiter + 1;
	}

	return parsedPaths;
}

// SerializeStyleSourceList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string SerializeStyleSourceList(const std::vector<std::string>& serializedPaths)
{
	std::ostringstream stream;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < serializedPaths.size(); ++index)
	{
		if (index > 0)
		{
			stream << ';';
		}
		stream << serializedPaths[index];
	}

	return stream.str();
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

// ReadFileToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ReadFileToString(const std::filesystem::path& filePath)
{
	// Read through ResourceManager so exported builds can load UI markup and
	// styles from the mounted TEPAK archive instead of assuming loose files are
	// present beside the executable on disk.
	const std::vector<std::uint8_t> fileBytes = ResourceManager::Instance().ReadFileBytes(filePath);
	if (fileBytes.empty())
	{
		// Zero-byte UI files on disk are valid authored placeholders. Preserve
		// the distinction between "empty file" and "missing file" so empty
		// stylesheets do not surface as load failures. Exported builds can also
		// author empty files inside the mounted archive, so check the mount too.
		if (std::filesystem::exists(filePath))
		{
			return {};
		}

		const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();
		const std::filesystem::path absolutePath = filePath.is_absolute()
			? filePath.lexically_normal()
			: (assetRoot / filePath).lexically_normal();
		const std::filesystem::path logicalPath = absolutePath.lexically_relative(assetRoot);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ContentMount::Instance().IsMounted()
			&& logicalPath != absolutePath
			&& ContentMount::Instance().Exists(logicalPath.generic_string()))
		{
			return {};
		}

		throw std::runtime_error("Failed to open UI asset: " + filePath.generic_string());
	}

	return std::string(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
}

// ParseFloatValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<float> ParseFloatValue(std::string_view text)
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

// ApplyStyleProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ApplyStyleProperty(UIStyle& style, std::string_view propertyName, std::string_view propertyValue)
{
	const std::string name = ToLowerCopy(TrimCopy(propertyName));
	const std::string value = TrimCopy(propertyValue);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name.empty() || value.empty())
	{
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "background-color")
	{
		style.backgroundColor = ParseUIColor(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "color")
	{
		style.color = ParseUIColor(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "opacity")
	{
		style.opacity = ParseFloatValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "font-size")
	{
		const std::optional<UIValue> parsed = ParseUIValue(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (parsed.has_value() && parsed->unit != UIValue::Unit::Auto)
		{
			style.fontSize = parsed->value;
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "font-family")
	{
		style.fontFamily = value;
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "flex-direction")
	{
		const std::string loweredValue = ToLowerCopy(value);
		style.flexDirection = loweredValue == "row" ? UIFlexDirection::Row : UIFlexDirection::Column;
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "justify-content")
	{
		const std::string loweredValue = ToLowerCopy(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (loweredValue == "center")
		{
			style.justifyContent = UIJustifyContent::Center;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (loweredValue == "flex-end")
		{
			style.justifyContent = UIJustifyContent::FlexEnd;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (loweredValue == "space-between")
		{
			style.justifyContent = UIJustifyContent::SpaceBetween;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (loweredValue == "space-around")
		{
			style.justifyContent = UIJustifyContent::SpaceAround;
		}
		else
		{
			style.justifyContent = UIJustifyContent::FlexStart;
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "align-items")
	{
		const std::string loweredValue = ToLowerCopy(value);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (loweredValue == "flex-start")
		{
			style.alignItems = UIAlignItems::FlexStart;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (loweredValue == "center")
		{
			style.alignItems = UIAlignItems::Center;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (loweredValue == "flex-end")
		{
			style.alignItems = UIAlignItems::FlexEnd;
		}
		else
		{
			style.alignItems = UIAlignItems::Stretch;
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "flex-grow")
	{
		style.flexGrow = ParseFloatValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "flex-shrink")
	{
		style.flexShrink = ParseFloatValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "flex-basis")
	{
		style.flexBasis = ParseUIValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "width")
	{
		style.width = ParseUIValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "height")
	{
		style.height = ParseUIValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "margin")
	{
		style.margin = ParseUIEdgeValues(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "padding")
	{
		style.padding = ParseUIEdgeValues(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "border-width")
	{
		style.borderWidth = ParseFloatValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "border-radius")
	{
		style.borderRadius = ParseFloatValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "translate")
	{
		style.transform.translate = ParseUIVector2(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "scale")
	{
		style.transform.scale = ParseFloatValue(value);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (name == "rotate")
	{
		style.transform.rotationDegrees = ParseFloatValue(value);
		return;
	}
}

// ParseInlineStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIStyle ParseInlineStyle(std::string_view inlineStyleText)
{
	UIStyle style;
	std::stringstream declarations{ std::string(inlineStyleText) };
	std::string declaration;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (std::getline(declarations, declaration, ';'))
	{
		const std::size_t separator = declaration.find(':');
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (separator == std::string::npos)
		{
			continue;
		}

		ApplyStyleProperty(
			style,
			std::string_view(declaration.data(), separator),
			std::string_view(declaration.data() + separator + 1, declaration.size() - separator - 1));
	}

	return style;
}

// ApplyDeclarationBlock is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ApplyDeclarationBlock(UIStyle& style, std::string_view blockBody)
{
	std::stringstream declarations{ std::string(blockBody) };
	std::string declaration;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (std::getline(declarations, declaration, ';'))
	{
		const std::size_t separator = declaration.find(':');
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (separator == std::string::npos)
		{
			continue;
		}

		ApplyStyleProperty(
			style,
			std::string_view(declaration.data(), separator),
			std::string_view(declaration.data() + separator + 1, declaration.size() - separator - 1));
	}
}

// TryParseSelectorRule is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool TryParseSelectorRule(std::string_view selectorText, std::size_t ruleOrder, UIStyleRule& outRule)
{
	const std::string trimmedSelector = TrimCopy(selectorText);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmedSelector.empty())
	{
		return false;
	}

	UIStyleRule rule;
	rule.selectorText = trimmedSelector;
	rule.order = ruleOrder;

	const std::string loweredSelector = ToLowerCopy(trimmedSelector);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (loweredSelector == ":root")
	{
		rule.selectorKind = UISelectorKind::Root;
		rule.selectorValue = ":root";
		rule.specificity = 1000;
		outRule = std::move(rule);
		return true;
	}

	const std::size_t pseudoSeparator = trimmedSelector.find(':');
	const std::string selectorCore = pseudoSeparator == std::string::npos ? trimmedSelector : trimmedSelector.substr(0, pseudoSeparator);
	const std::string pseudoText = pseudoSeparator == std::string::npos ? std::string{} : trimmedSelector.substr(pseudoSeparator + 1);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!pseudoText.empty() && ToLowerCopy(pseudoText) == "hover")
	{
		rule.pseudoState = UIPseudoState::Hover;
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (!pseudoText.empty())
	{
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!selectorCore.empty() && selectorCore[0] == '#')
	{
		rule.selectorKind = UISelectorKind::Id;
		rule.selectorValue = selectorCore.substr(1);
		rule.specificity = 100;
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (!selectorCore.empty() && selectorCore[0] == '.')
	{
		rule.selectorKind = UISelectorKind::Class;
		rule.selectorValue = selectorCore.substr(1);
		rule.specificity = 10;
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (!selectorCore.empty())
	{
		rule.selectorKind = UISelectorKind::Type;
		rule.selectorValue = selectorCore;
		rule.specificity = 1;
	}
	else
	{
		return false;
	}

	outRule = std::move(rule);
	return true;
}

// ParseXmlNodeRecursive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr ParseXmlNodeRecursive(const pugi::xml_node& xmlNode)
{
	const UIElementType elementType = UIElementTypeFromName(xmlNode.name());
	UIElement::Ptr element = UIElement::Create(elementType);
	element->typeName = xmlNode.name();

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const pugi::xml_attribute& attribute : xmlNode.attributes())
	{
		const std::string name = attribute.name();
		const std::string value = attribute.value();
		element->attributes[name] = value;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (name == "id")
		{
			element->id = value;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (name == "class")
		{
			element->SetClassListFromString(value);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (name == "text")
		{
			element->text = value;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (name == "src")
		{
			element->source = value;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (name == "styleSrc")
		{
			element->styleSheetSources = ParseStyleSourceList(value);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (name == "styleSrcs")
		{
			element->styleSheetSources = ParseStyleSourceList(value);
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (name == "style")
		{
			element->inlineStyle = ParseInlineStyle(value);
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (element->text.empty())
	{
		element->text = TrimCopy(xmlNode.child_value());
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const pugi::xml_node& childNode : xmlNode.children())
	{
		if (childNode.type() != pugi::node_element)
		{
			continue;
		}

		element->AddChild(ParseXmlNodeRecursive(childNode));
	}

	return element;
}

// MatchesRule is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MatchesRule(const UIStyleRule& rule, const UIElement& element)
{
	switch (rule.selectorKind)
	{
	case UISelectorKind::Root:
		return element.type == UIElementType::Root;
	case UISelectorKind::Id:
		return element.id == rule.selectorValue;
	case UISelectorKind::Class:
		return element.HasClass(rule.selectorValue);
	case UISelectorKind::Type:
	default:
		return ToLowerCopy(element.typeName) == ToLowerCopy(rule.selectorValue);
	}
}

// ColorToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ColorToString(const glm::vec4& color)
{
	std::ostringstream stream;
	stream << "rgba("
		<< static_cast<int>(std::round(color.r * 255.0f)) << ", "
		<< static_cast<int>(std::round(color.g * 255.0f)) << ", "
		<< static_cast<int>(std::round(color.b * 255.0f)) << ", "
		<< std::fixed << std::setprecision(2) << color.a
		<< ")";
	return stream.str();
}

// ValueToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ValueToString(const UIValue& value)
{
	switch (value.unit)
	{
	case UIValue::Unit::Percent:
		return std::to_string(value.value) + "%";
	case UIValue::Unit::Auto:
		return "auto";
	case UIValue::Unit::Points:
	default:
		return std::to_string(value.value) + "px";
	}
}

// EdgeValuesToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string EdgeValuesToString(const UIEdgeValues& values)
{
	return ValueToString(values.top) + " " + ValueToString(values.right) + " " + ValueToString(values.bottom) + " " + ValueToString(values.left);
}

// SelectorToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string SelectorToString(const UIStyleRule& rule)
{
	if (!rule.selectorText.empty())
	{
		return rule.selectorText;
	}

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (rule.selectorKind)
	{
	case UISelectorKind::Root:
		return ":root";
	case UISelectorKind::Id:
		return "#" + rule.selectorValue;
	case UISelectorKind::Class:
		return "." + rule.selectorValue;
	case UISelectorKind::Type:
	default:
		return rule.selectorValue;
	}
}

// HasSerializedStyleProperties is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool HasSerializedStyleProperties(const UIStyle& style)
{
	return style.HasRenderableBackground()
		|| style.color.has_value()
		|| style.opacity.has_value()
		|| style.fontSize.has_value()
		|| style.fontFamily.has_value()
		|| style.width.has_value()
		|| style.height.has_value()
		|| style.flexDirection.has_value()
		|| style.justifyContent.has_value()
		|| style.alignItems.has_value()
		|| style.flexGrow.has_value()
		|| style.flexShrink.has_value()
		|| style.flexBasis.has_value()
		|| style.padding.has_value()
		|| style.margin.has_value()
		|| style.borderWidth.has_value()
		|| style.borderRadius.has_value()
		|| style.transform.translate.has_value()
		|| style.transform.scale.has_value()
		|| style.transform.rotationDegrees.has_value();
}

// EmitStyleProperties is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EmitStyleProperties(const UIStyle& style, std::ostringstream& stream, int depth)
{
	const std::string indent(static_cast<std::size_t>(depth), '\t');

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.backgroundColor.has_value())
	{
		stream << indent << "background-color: " << ColorToString(*style.backgroundColor) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.color.has_value())
	{
		stream << indent << "color: " << ColorToString(*style.color) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.opacity.has_value())
	{
		stream << indent << "opacity: " << *style.opacity << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.fontSize.has_value())
	{
		stream << indent << "font-size: " << *style.fontSize << "px;\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.fontFamily.has_value())
	{
		stream << indent << "font-family: " << *style.fontFamily << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexDirection.has_value())
	{
		stream << indent << "flex-direction: " << (*style.flexDirection == UIFlexDirection::Row ? "row" : "column") << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.justifyContent.has_value())
	{
		switch (*style.justifyContent)
		{
		case UIJustifyContent::Center:
			stream << indent << "justify-content: center;\n";
			break;
		case UIJustifyContent::FlexEnd:
			stream << indent << "justify-content: flex-end;\n";
			break;
		case UIJustifyContent::SpaceBetween:
			stream << indent << "justify-content: space-between;\n";
			break;
		case UIJustifyContent::SpaceAround:
			stream << indent << "justify-content: space-around;\n";
			break;
		case UIJustifyContent::FlexStart:
		default:
			stream << indent << "justify-content: flex-start;\n";
			break;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.alignItems.has_value())
	{
		switch (*style.alignItems)
		{
		case UIAlignItems::FlexStart:
			stream << indent << "align-items: flex-start;\n";
			break;
		case UIAlignItems::Center:
			stream << indent << "align-items: center;\n";
			break;
		case UIAlignItems::FlexEnd:
			stream << indent << "align-items: flex-end;\n";
			break;
		case UIAlignItems::Stretch:
		default:
			stream << indent << "align-items: stretch;\n";
			break;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexGrow.has_value())
	{
		stream << indent << "flex-grow: " << *style.flexGrow << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexShrink.has_value())
	{
		stream << indent << "flex-shrink: " << *style.flexShrink << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexBasis.has_value())
	{
		stream << indent << "flex-basis: " << ValueToString(*style.flexBasis) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.width.has_value())
	{
		stream << indent << "width: " << ValueToString(*style.width) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.height.has_value())
	{
		stream << indent << "height: " << ValueToString(*style.height) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.margin.has_value())
	{
		stream << indent << "margin: " << EdgeValuesToString(*style.margin) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.padding.has_value())
	{
		stream << indent << "padding: " << EdgeValuesToString(*style.padding) << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.borderWidth.has_value())
	{
		stream << indent << "border-width: " << *style.borderWidth << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.borderRadius.has_value())
	{
		stream << indent << "border-radius: " << *style.borderRadius << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.transform.translate.has_value())
	{
		stream << indent << "translate: " << style.transform.translate->x << " " << style.transform.translate->y << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.transform.scale.has_value())
	{
		stream << indent << "scale: " << *style.transform.scale << ";\n";
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.transform.rotationDegrees.has_value())
	{
		stream << indent << "rotate: " << *style.transform.rotationDegrees << ";\n";
	}
}

// SerializeInlineStyleAttribute is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string SerializeInlineStyleAttribute(const UIStyle& style)
{
	std::ostringstream stream;
	bool emittedAnyProperty = false;

	const auto appendProperty = [&](std::string_view name, const std::string& value) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (emittedAnyProperty)
		{
			stream << ' ';
		}

		stream << name << ": " << value << ";";
		emittedAnyProperty = true;
	};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.backgroundColor.has_value())
	{
		appendProperty("background-color", ColorToString(*style.backgroundColor));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.color.has_value())
	{
		appendProperty("color", ColorToString(*style.color));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.opacity.has_value())
	{
		appendProperty("opacity", std::to_string(*style.opacity));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.fontSize.has_value())
	{
		appendProperty("font-size", std::to_string(*style.fontSize) + "px");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.fontFamily.has_value())
	{
		appendProperty("font-family", *style.fontFamily);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexDirection.has_value())
	{
		appendProperty("flex-direction", *style.flexDirection == UIFlexDirection::Row ? "row" : "column");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.justifyContent.has_value())
	{
		switch (*style.justifyContent)
		{
		case UIJustifyContent::Center:
			appendProperty("justify-content", "center");
			break;
		case UIJustifyContent::FlexEnd:
			appendProperty("justify-content", "flex-end");
			break;
		case UIJustifyContent::SpaceBetween:
			appendProperty("justify-content", "space-between");
			break;
		case UIJustifyContent::SpaceAround:
			appendProperty("justify-content", "space-around");
			break;
		case UIJustifyContent::FlexStart:
		default:
			appendProperty("justify-content", "flex-start");
			break;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.alignItems.has_value())
	{
		switch (*style.alignItems)
		{
		case UIAlignItems::FlexStart:
			appendProperty("align-items", "flex-start");
			break;
		case UIAlignItems::Center:
			appendProperty("align-items", "center");
			break;
		case UIAlignItems::FlexEnd:
			appendProperty("align-items", "flex-end");
			break;
		case UIAlignItems::Stretch:
		default:
			appendProperty("align-items", "stretch");
			break;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexGrow.has_value())
	{
		appendProperty("flex-grow", std::to_string(*style.flexGrow));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexShrink.has_value())
	{
		appendProperty("flex-shrink", std::to_string(*style.flexShrink));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexBasis.has_value())
	{
		appendProperty("flex-basis", ValueToString(*style.flexBasis));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.width.has_value())
	{
		appendProperty("width", ValueToString(*style.width));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.height.has_value())
	{
		appendProperty("height", ValueToString(*style.height));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.margin.has_value())
	{
		appendProperty("margin", EdgeValuesToString(*style.margin));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.padding.has_value())
	{
		appendProperty("padding", EdgeValuesToString(*style.padding));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.borderWidth.has_value())
	{
		appendProperty("border-width", std::to_string(*style.borderWidth));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.borderRadius.has_value())
	{
		appendProperty("border-radius", std::to_string(*style.borderRadius));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.transform.translate.has_value())
	{
		appendProperty(
			"translate",
			std::to_string(style.transform.translate->x) + " " + std::to_string(style.transform.translate->y));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.transform.scale.has_value())
	{
		appendProperty("scale", std::to_string(*style.transform.scale));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.transform.rotationDegrees.has_value())
	{
		appendProperty("rotate", std::to_string(*style.transform.rotationDegrees));
	}

	return stream.str();
}
}

// UIParser::ParseMarkupFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr UIParser::ParseMarkupFile(const std::filesystem::path& markupPath) const
{
	return ParseMarkupString(ReadFileToString(markupPath), markupPath.generic_string());
}

// UIParser::TryParseMarkupFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIParser::TryParseMarkupFile(const std::filesystem::path& markupPath, UIElement::Ptr& outRoot, std::string& outError) const
{
	try
	{
		outRoot = ParseMarkupFile(markupPath);
		outError.clear();
		return true;
	}
	catch (const std::exception& exception)
	{
		outRoot.reset();
		outError = exception.what();
		return false;
	}
}

// UIParser::ParseMarkupString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr UIParser::ParseMarkupString(std::string_view markupText, std::string_view debugName) const
{
	pugi::xml_document document;
	const pugi::xml_parse_result result = document.load_string(std::string(markupText).c_str());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result)
	{
		throw std::runtime_error("Failed to parse " + std::string(debugName) + ": " + result.description());
	}

	const pugi::xml_node rootNode = document.document_element();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!rootNode)
	{
		throw std::runtime_error("UI markup is empty: " + std::string(debugName));
	}

	UIElement::Ptr root = ParseXmlNodeRecursive(rootNode);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (root->type != UIElementType::Root)
	{
		UIElement::Ptr wrappedRoot = UIElement::Create(UIElementType::Root);
		wrappedRoot->typeName = "UIRoot";
		wrappedRoot->AddChild(root);
		root = wrappedRoot;
	}

	return root;
}

// UIParser::ParseStyleFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<UIStyleRule> UIParser::ParseStyleFile(const std::filesystem::path& stylePath) const
{
	return ParseStyleString(ReadFileToString(stylePath));
}

// UIParser::TryParseStyleFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIParser::TryParseStyleFile(const std::filesystem::path& stylePath, std::vector<UIStyleRule>& outRules, std::string& outError) const
{
	try
	{
		outRules = ParseStyleFile(stylePath);
		outError.clear();
		return true;
	}
	catch (const std::exception& exception)
	{
		outRules.clear();
		outError = exception.what();
		return false;
	}
}

// UIParser::ParseStyleString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<UIStyleRule> UIParser::ParseStyleString(std::string_view styleText) const
{
	std::vector<UIStyleRule> rules;
	std::string remaining(styleText);
	std::size_t ruleOrder = 0;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (!remaining.empty())
	{
		const std::size_t blockStart = remaining.find('{');
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (blockStart == std::string::npos)
		{
			break;
		}

		const std::size_t blockEnd = remaining.find('}', blockStart);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (blockEnd == std::string::npos)
		{
			break;
		}

		const std::string selectorText = TrimCopy(std::string_view(remaining.data(), blockStart));
		const std::string blockBody = remaining.substr(blockStart + 1, blockEnd - blockStart - 1);
		remaining = remaining.substr(blockEnd + 1);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (selectorText.empty())
		{
			continue;
		}

		UIStyleRule rule;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryParseSelectorRule(selectorText, ruleOrder++, rule))
		{
			// Keep unsupported selectors in memory so the builder can show and
			// repair them instead of silently deleting authored stylesheet data.
			rule.selectorText = selectorText;
			rule.order = rules.size();
			rule.validity = UISelectorValidityState::RecoverableError;
			rule.validationMessage = "Unsupported selector. Use :root, element names, #ids, .classes, and optional :hover.";
			ApplyDeclarationBlock(rule.style, blockBody);
			rules.push_back(std::move(rule));
			continue;
		}

		ApplyDeclarationBlock(rule.style, blockBody);

		rules.push_back(std::move(rule));
	}

	return rules;
}

// UIParser::TryParseSelector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIParser::TryParseSelector(std::string_view selectorText, UIStyleRule& outRule) const
{
	return TryParseSelectorRule(selectorText, 0, outRule);
}

// UIParser::ApplyStyles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIParser::ApplyStyles(const UIElement::Ptr& root, const std::vector<UIStyleRule>& rules) const
{
	if (!root)
	{
		return;
	}

	const auto applyRecursive = [&](const auto& self, const UIElement::Ptr& element) -> void {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element)
		{
			return;
		}

		std::vector<const UIStyleRule*> matchingRules;
		matchingRules.reserve(rules.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIStyleRule& rule : rules)
		{
			if (rule.validity != UISelectorValidityState::Valid)
			{
				// Invalid rules stay editable in the builder, but runtime styling
				// ignores them until the selector text is repaired.
				continue;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (MatchesRule(rule, *element))
			{
				matchingRules.push_back(&rule);
			}
		}

		std::sort(matchingRules.begin(), matchingRules.end(), [](const UIStyleRule* left, const UIStyleRule* right) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (left->specificity != right->specificity)
			{
				return left->specificity < right->specificity;
			}
			return left->order < right->order;
		});

		UIStyle computedStyle;
		UIStyle hoverStyle;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIStyleRule* rule : matchingRules)
		{
			if (rule->pseudoState == UIPseudoState::Hover)
			{
				hoverStyle.MergeFrom(rule->style);
			}
			else
			{
				computedStyle.MergeFrom(rule->style);
			}
		}

		computedStyle.MergeFrom(element->inlineStyle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (element->type == UIElementType::Root)
		{
			if (!computedStyle.width.has_value())
			{
				computedStyle.width = UIValue::Percent(100.0f);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!computedStyle.height.has_value())
			{
				computedStyle.height = UIValue::Percent(100.0f);
			}
		}

		element->computedStyle = computedStyle;
		element->hoverStyle = hoverStyle;
		element->RefreshDisplayStyle();

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIElement::Ptr& child : element->children)
		{
			self(self, child);
		}
	};
	applyRecursive(applyRecursive, root);
}

// UIParser::SerializeMarkup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UIParser::SerializeMarkup(const UIElement::Ptr& root) const
{
	if (!root)
	{
		return {};
	}

	const auto emitRecursive = [&](const auto& self, const UIElement& element, std::ostringstream& stream, int depth) -> void {
		const std::string indent(static_cast<std::size_t>(depth), '\t');
		stream << indent << "<" << element.typeName;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element.id.empty())
		{
			stream << " id=\"" << element.id << "\"";
		}

		const std::string classText = element.GetClassListAsString();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!classText.empty())
		{
			stream << " class=\"" << classText << "\"";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element.text.empty())
		{
			stream << " text=\"" << element.text << "\"";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element.source.empty())
		{
			stream << " src=\"" << element.source << "\"";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (element.styleSheetSources.size() == 1)
		{
			stream << " styleSrc=\"" << element.styleSheetSources.front() << "\"";
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (!element.styleSheetSources.empty())
		{
			stream << " styleSrcs=\"" << SerializeStyleSourceList(element.styleSheetSources) << "\"";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (HasSerializedStyleProperties(element.inlineStyle))
		{
			stream << " style=\"" << SerializeInlineStyleAttribute(element.inlineStyle) << "\"";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (element.children.empty())
		{
			stream << " />\n";
			return;
		}

		stream << ">\n";
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIElement::Ptr& child : element.children)
		{
			if (child)
			{
				self(self, *child, stream, depth + 1);
			}
		}
		stream << indent << "</" << element.typeName << ">\n";
	};

	std::ostringstream output;
	emitRecursive(emitRecursive, *root, output, 0);
	return output.str();
}

// UIParser::SerializeStyles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UIParser::SerializeStyles(const UIElement::Ptr& root) const
{
	if (!root)
	{
		return {};
	}

	const auto emitRecursive = [&](const auto& self, const UIElement::Ptr& element, std::ostringstream& stream) -> void {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element)
		{
			return;
		}

		std::string selector;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (element->type == UIElementType::Root)
		{
			selector = ":root";
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (!element->id.empty())
		{
			selector = "#" + element->id;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (!element->classList.empty())
		{
			selector = "." + element->classList.front();
		}
		else
		{
			selector = element->typeName;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (HasSerializedStyleProperties(element->computedStyle))
		{
			stream << selector << " {\n";
			EmitStyleProperties(element->computedStyle, stream, 1);
			stream << "}\n\n";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (HasSerializedStyleProperties(element->hoverStyle))
		{
			stream << selector << ":hover {\n";
			EmitStyleProperties(element->hoverStyle, stream, 1);
			stream << "}\n\n";
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIElement::Ptr& child : element->children)
		{
			self(self, child, stream);
		}
	};

	std::ostringstream output;
	emitRecursive(emitRecursive, root, output);
	return output.str();
}

// UIParser::SerializeStyles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UIParser::SerializeStyles(const std::vector<UIStyleRule>& rules) const
{
	std::ostringstream output;
	bool emittedAnyRule = false;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const UIStyleRule& rule : rules)
	{
		if (!HasSerializedStyleProperties(rule.style))
		{
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (emittedAnyRule)
		{
			output << "\n";
		}

		output << SelectorToString(rule) << " {\n";
		EmitStyleProperties(rule.style, output, 1);
		output << "}\n";
		emittedAnyRule = true;
	}

	return output.str();
}
