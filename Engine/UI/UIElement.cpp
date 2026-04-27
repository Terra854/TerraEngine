// UIElement.cpp — Implementation of the UI visual tree node logic.
//
// This file contains the implementation for managing UI hierarchies, cloning
// elements, and handling dirty state propagation. It also manages the
// transition animations between normal and hover states for UI elements.

#include "UIElement.h"

#include <algorithm>
#include <sstream>

namespace
{
constexpr float kHoverAnimationSpeed = 6.0f;
}

// UIElement::Create is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr UIElement::Create(UIElementType nextType)
{
	return std::make_shared<UIElement>(nextType);
}

// UIElement::CreateDefault is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr UIElement::CreateDefault(UIElementType nextType)
{
	Ptr element = Create(nextType);
	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (nextType)
	{
	case UIElementType::Root:
		element->typeName = "UIRoot";
		break;
	case UIElementType::Label:
		element->text = "Label";
		break;
	case UIElementType::Button:
		element->text = "Button";
		break;
	case UIElementType::Image:
		element->source = "Textures/";
		break;
	case UIElementType::VisualElement:
	default:
		break;
	}

	return element;
}

UIElement::UIElement(UIElementType nextType)
	// type is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: type(nextType)
	, typeName(ToString(nextType))
{
	RefreshDisplayStyle();
}

// UIElement::AddChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::AddChild(const Ptr& child)
{
	InsertChild(children.size(), child);
}

// UIElement::InsertChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::InsertChild(std::size_t index, const Ptr& child)
{
	if (!child)
	{
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (child.get() == this || child->IsAncestorOf(this))
	{
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (child->parent != nullptr && child->parent != this)
	{
		child->parent->RemoveChild(child);
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (child->parent == this)
	{
		RemoveChild(child);
	}

	child->parent = this;
	index = std::min(index, children.size());
	children.insert(children.begin() + static_cast<std::ptrdiff_t>(index), child);
	MarkDirty();
	return true;
}

// UIElement::RemoveChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::RemoveChild(const Ptr& child)
{
	if (!child)
	{
		return false;
	}

	const auto iterator = std::find(children.begin(), children.end(), child);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (iterator == children.end())
	{
		return false;
	}

	(*iterator)->parent = nullptr;
	children.erase(iterator);
	MarkDirty();
	return true;
}

// UIElement::MoveChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::MoveChild(std::size_t fromIndex, std::size_t toIndex)
{
	if (fromIndex >= children.size() || toIndex >= children.size() || fromIndex == toIndex)
	{
		return false;
	}

	Ptr movedChild = children[fromIndex];
	children.erase(children.begin() + static_cast<std::ptrdiff_t>(fromIndex));
	children.insert(children.begin() + static_cast<std::ptrdiff_t>(toIndex), std::move(movedChild));
	MarkDirty();
	return true;
}

// UIElement::ReparentTo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::ReparentTo(const Ptr& newParent, std::size_t insertIndex)
{
	if (!newParent || newParent.get() == this || IsAncestorOf(newParent.get()))
	{
		return false;
	}

	Ptr self = shared_from_this();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (parent != nullptr)
	{
		parent->RemoveChild(self);
	}

	const std::size_t resolvedIndex = insertIndex == std::numeric_limits<std::size_t>::max()
		? newParent->children.size()
		: insertIndex;
	return newParent->InsertChild(resolvedIndex, self);
}

// UIElement::ClearChildren is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::ClearChildren()
{
	for (const Ptr& child : children)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (child)
		{
			child->parent = nullptr;
		}
	}
	children.clear();
	dirty = true;
}

// UIElement::Clone is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr UIElement::Clone() const
{
	Ptr clone = Create(type);
	clone->typeName = typeName;
	clone->id = id;
	clone->classList = classList;
	clone->text = text;
	clone->source = source;
	clone->styleSheetSources = styleSheetSources;
	clone->attributes = attributes;
	clone->inlineStyle = inlineStyle;
	clone->computedStyle = computedStyle;
	clone->hoverStyle = hoverStyle;
	clone->displayStyle = displayStyle;
	clone->calculatedPosition = calculatedPosition;
	clone->calculatedSize = calculatedSize;
	clone->dirty = dirty;
	clone->visible = visible;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const Ptr& child : children)
	{
		if (!child)
		{
			continue;
		}

		Ptr childClone = child->Clone();
		childClone->parent = clone.get();
		clone->children.push_back(childClone);
	}

	return clone;
}

// UIElement::FindById is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIElement::Ptr UIElement::FindById(std::string_view wantedId)
{
	if (id == wantedId)
	{
		return shared_from_this();
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const Ptr& child : children)
	{
		if (!child)
		{
			continue;
		}

		Ptr found = child->FindById(wantedId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (found)
		{
			return found;
		}
	}

	return nullptr;
}

// UIElement::HasClass is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::HasClass(std::string_view className) const
{
	return std::find(classList.begin(), classList.end(), className) != classList.end();
}

// UIElement::IsAncestorOf is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::IsAncestorOf(const UIElement* candidate) const
{
	const UIElement* current = candidate;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (current != nullptr)
	{
		if (current == this)
		{
			return true;
		}
		current = current->parent;
	}

	return false;
}

// UIElement::GetDisplayLabel is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UIElement::GetDisplayLabel() const
{
	if (!id.empty())
	{
		return typeName + " [" + id + "]";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!text.empty())
	{
		return typeName + " \"" + text + "\"";
	}

	return typeName;
}

// UIElement::SetClassListFromString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::SetClassListFromString(std::string_view classText)
{
	classList.clear();

	std::stringstream stream{ std::string(classText) };
	std::string token;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (stream >> token)
	{
		classList.push_back(token);
	}

	dirty = true;
}

// UIElement::GetClassListAsString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string UIElement::GetClassListAsString() const
{
	std::string joined;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < classList.size(); ++index)
	{
		if (index > 0)
		{
			joined += ' ';
		}
		joined += classList[index];
	}
	return joined;
}

// UIElement::MarkDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::MarkDirty()
{
	dirty = true;

	UIElement* current = parent;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (current != nullptr)
	{
		current->dirty = true;
		current = current->parent;
	}
}

// UIElement::ClearDirtyRecursive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::ClearDirtyRecursive()
{
	dirty = false;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const Ptr& child : children)
	{
		if (child)
		{
			child->ClearDirtyRecursive();
		}
	}
}

// UIElement::SetVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::SetVisible(bool nextVisible)
{
	if (visible == nextVisible)
	{
		return;
	}

	visible = nextVisible;
	MarkDirty();
}

// UIElement::IsVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::IsVisible() const
{
	return visible;
}

// UIElement::SetHoverState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::SetHoverState(bool hovered)
{
	hoverActive_ = hovered;
}

// UIElement::AdvanceAnimation is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::AdvanceAnimation(float deltaTime)
{
	const float previousBlend = hoverBlend_;
	const float targetBlend = hoverActive_ ? 1.0f : 0.0f;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hoverBlend_ < targetBlend)
	{
		hoverBlend_ = std::min(targetBlend, hoverBlend_ + (deltaTime * kHoverAnimationSpeed));
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (hoverBlend_ > targetBlend)
	{
		hoverBlend_ = std::max(targetBlend, hoverBlend_ - (deltaTime * kHoverAnimationSpeed));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::abs(hoverBlend_ - previousBlend) > 0.0001f)
	{
		RefreshDisplayStyle();
		return true;
	}

	return false;
}

// UIElement::AdvanceAnimationRecursive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UIElement::AdvanceAnimationRecursive(float deltaTime)
{
	bool changed = AdvanceAnimation(deltaTime);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const Ptr& child : children)
	{
		if (child)
		{
			changed = child->AdvanceAnimationRecursive(deltaTime) || changed;
		}
	}
	return changed;
}

// UIElement::BuildTargetDisplayStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
UIStyle UIElement::BuildTargetDisplayStyle() const
{
	UIStyle target = computedStyle;
	target.MergeFrom(hoverStyle);
	return target;
}

// UIElement::RefreshDisplayStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UIElement::RefreshDisplayStyle()
{
	displayStyle = UIStyle::Lerp(computedStyle, BuildTargetDisplayStyle(), hoverBlend_);
}
