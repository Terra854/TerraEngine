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

enum class UISelectorKind
{
	Root,
	Type,
	Class,
	Id,
};

enum class UIPseudoState
{
	None,
	Hover,
};

enum class UISelectorValidityState
{
	Valid,
	RecoverableError,
};

enum class UIFlexDirection
{
	Row,
	Column,
};

enum class UIJustifyContent
{
	FlexStart,
	Center,
	FlexEnd,
	SpaceBetween,
	SpaceAround,
};

enum class UIAlignItems
{
	Stretch,
	FlexStart,
	Center,
	FlexEnd,
};

// Numeric layout values can be authored as points, percentages, or `auto`.
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
