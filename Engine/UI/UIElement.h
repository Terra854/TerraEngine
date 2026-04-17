#pragma once

#include "UIStyle.h"

#include <glm/vec2.hpp>

#include <memory>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// UIElement is the in-memory visual tree node shared by the parser, layout
// system, renderer, and editor builder.
class UIElement : public std::enable_shared_from_this<UIElement>
{
public:
	using Ptr = std::shared_ptr<UIElement>;
	using UIEventCallback = void (*)(UIElement&, const glm::vec2&);

	static Ptr Create(UIElementType type);
	static Ptr CreateDefault(UIElementType type);

	explicit UIElement(UIElementType type = UIElementType::VisualElement);

	void AddChild(const Ptr& child);
	bool InsertChild(std::size_t index, const Ptr& child);
	bool RemoveChild(const Ptr& child);
	bool MoveChild(std::size_t fromIndex, std::size_t toIndex);
	bool ReparentTo(const Ptr& newParent, std::size_t insertIndex = std::numeric_limits<std::size_t>::max());
	void ClearChildren();

	[[nodiscard]] Ptr Clone() const;
	[[nodiscard]] Ptr FindById(std::string_view wantedId);
	[[nodiscard]] bool HasClass(std::string_view className) const;
	[[nodiscard]] bool IsAncestorOf(const UIElement* candidate) const;
	[[nodiscard]] std::string GetDisplayLabel() const;

	void SetClassListFromString(std::string_view classText);
	[[nodiscard]] std::string GetClassListAsString() const;

	void MarkDirty();
	void ClearDirtyRecursive();
	void SetVisible(bool visible);
	[[nodiscard]] bool IsVisible() const;
	void SetHoverState(bool hovered);
	[[nodiscard]] bool AdvanceAnimation(float deltaTime);
	[[nodiscard]] bool AdvanceAnimationRecursive(float deltaTime);
	[[nodiscard]] UIStyle BuildTargetDisplayStyle() const;
	void RefreshDisplayStyle();

	UIElementType type = UIElementType::VisualElement;
	std::string typeName = "VisualElement";
	std::string id;
	std::vector<std::string> classList;
	std::string text;
	std::string source;
	std::vector<std::string> styleSheetSources;
	std::unordered_map<std::string, std::string> attributes;
	UIStyle inlineStyle;
	UIStyle computedStyle;
	UIStyle hoverStyle;
	UIStyle displayStyle;
	std::vector<Ptr> children;
	UIElement* parent = nullptr;
	void* yogaNode = nullptr;
	glm::vec2 calculatedPosition = glm::vec2(0.0f, 0.0f);
	glm::vec2 calculatedSize = glm::vec2(0.0f, 0.0f);
	bool dirty = true;
	bool visible = true;
	UIEventCallback OnClick = nullptr;
	UIEventCallback OnMouseEnter = nullptr;
	UIEventCallback OnMouseLeave = nullptr;

private:
	float hoverBlend_ = 0.0f;
	bool hoverActive_ = false;
};
