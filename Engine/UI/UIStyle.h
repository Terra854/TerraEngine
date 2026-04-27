// UIStyle.h — Core data structures for UI styling and layout properties.
//
// This file defines the UIStyle structure and associated enums that represent
// CSS-like properties. It includes support for layout (flexbox), visual
// appearance (colors, borders), and transforms, along with helper types for
// values (pixels vs percentages) and edge offsets.

#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string>
#include <string_view>

// UI elements use the authored tag names from `.tuiml` so the parser, layout
// engine, renderer, and editor builder all speak the same vocabulary.
enum class UIElementType
{
	Root,
	VisualElement,
	Label,
	Button,
	Image,
};

// UISelectorKind names the supported states explicitly so serialization and switches stay readable.
enum class UISelectorKind
{
	Root,
	Type,
	Class,
	Id,
};

// UIPseudoState names the supported states explicitly so serialization and switches stay readable.
enum class UIPseudoState
{
	None,
	Hover,
};

// UISelectorValidityState names the supported states explicitly so serialization and switches stay readable.
enum class UISelectorValidityState
{
	Valid,
	RecoverableError,
};

// UIFlexDirection names the supported states explicitly so serialization and switches stay readable.
enum class UIFlexDirection
{
	Row,
	Column,
};

// UIJustifyContent names the supported states explicitly so serialization and switches stay readable.
enum class UIJustifyContent
{
	FlexStart,
	Center,
	FlexEnd,
	SpaceBetween,
	SpaceAround,
};

// UIAlignItems names the supported states explicitly so serialization and switches stay readable.
enum class UIAlignItems
{
	Stretch,
	FlexStart,
	Center,
	FlexEnd,
};

// Numeric layout values can be authored as points, percentages, or `auto`.
// struct UIValue groups related state and operations so callers have one documented contract.
struct UIValue
{
	enum class Unit
	{
		Auto,
		Points,
		Percent,
	};

	float value = 0.0f;
	Unit unit = Unit::Auto;

	static UIValue Auto();
	static UIValue Points(float value);
	static UIValue Percent(float value);

	[[nodiscard]] bool IsAuto() const;
};

// Edge values are stored in top/right/bottom/left order so CSS-like shorthands
// map directly onto the internal representation.
struct UIEdgeValues
{
	UIValue top = UIValue::Points(0.0f);
	UIValue right = UIValue::Points(0.0f);
	UIValue bottom = UIValue::Points(0.0f);
	UIValue left = UIValue::Points(0.0f);

	static UIEdgeValues All(const UIValue& value);
};

// struct UITransformProperties groups related state and operations so callers have one documented contract.
struct UITransformProperties
{
	std::optional<glm::vec2> translate;
	std::optional<float> scale;
	std::optional<float> rotationDegrees;
};

// UIStyle intentionally keeps properties optional so authored rules can merge
// in CSS order without stomping defaults that were never specified.
struct UIStyle
{
	std::optional<glm::vec4> backgroundColor;
	std::optional<glm::vec4> color;
	std::optional<float> opacity;
	std::optional<float> fontSize;
	std::optional<std::string> fontFamily;
	std::optional<UIFlexDirection> flexDirection;
	std::optional<UIJustifyContent> justifyContent;
	std::optional<UIAlignItems> alignItems;
	std::optional<float> flexGrow;
	std::optional<float> flexShrink;
	std::optional<UIValue> flexBasis;
	std::optional<UIValue> width;
	std::optional<UIValue> height;
	std::optional<UIEdgeValues> margin;
	std::optional<UIEdgeValues> padding;
	std::optional<float> borderWidth;
	std::optional<float> borderRadius;
	UITransformProperties transform;

	void MergeFrom(const UIStyle& source);
	[[nodiscard]] static UIStyle Lerp(const UIStyle& from, const UIStyle& to, float alpha);
	[[nodiscard]] bool HasRenderableBackground() const;
};

// struct UIStyleRule groups related state and operations so callers have one documented contract.
struct UIStyleRule
{
	std::string selectorText;
	std::string selectorValue;
	UISelectorKind selectorKind = UISelectorKind::Type;
	UIPseudoState pseudoState = UIPseudoState::None;
	UIStyle style;
	int specificity = 0;
	std::size_t order = 0;
	UISelectorValidityState validity = UISelectorValidityState::Valid;
	std::string validationMessage;
};

std::optional<glm::vec4> ParseUIColor(std::string_view text);
std::optional<UIValue> ParseUIValue(std::string_view text);
std::optional<glm::vec2> ParseUIVector2(std::string_view text);
std::optional<UIEdgeValues> ParseUIEdgeValues(std::string_view text);

std::string ToString(UIElementType type);
UIElementType UIElementTypeFromName(std::string_view name);
