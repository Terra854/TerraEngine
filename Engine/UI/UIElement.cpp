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


UIElement::Ptr UIElement::Create(UIElementType nextType)
{
	return std::make_shared<UIElement>(nextType);
}


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

	: type(nextType)
	, typeName(ToString(nextType))
{
	RefreshDisplayStyle();
}


void UIElement::AddChild(const Ptr& child)
{
	InsertChild(children.size(), child);
}

// Reject self/ancestor links before detaching the child to keep the tree acyclic.
bool UIElement::InsertChild(std::size_t index, const Ptr& child)
{
	if (!child)
	{
		return false;
	}


	if (child.get() == this || child->IsAncestorOf(this))
	{
		return false;
	}


	if (child->parent != nullptr && child->parent != this)
	{
		child->parent->RemoveChild(child);
	}

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


bool UIElement::RemoveChild(const Ptr& child)
{
	if (!child)
	{
		return false;
	}

	const auto iterator = std::find(children.begin(), children.end(), child);

	if (iterator == children.end())
	{
		return false;
	}

	(*iterator)->parent = nullptr;
	children.erase(iterator);
	MarkDirty();
	return true;
}


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


bool UIElement::ReparentTo(const Ptr& newParent, std::size_t insertIndex)
{
	if (!newParent || newParent.get() == this || IsAncestorOf(newParent.get()))
	{
		return false;
	}

	Ptr self = shared_from_this();

	if (parent != nullptr)
	{
		parent->RemoveChild(self);
	}

	const std::size_t resolvedIndex = insertIndex == std::numeric_limits<std::size_t>::max()
		? newParent->children.size()
		: insertIndex;
	return newParent->InsertChild(resolvedIndex, self);
}


void UIElement::ClearChildren()
{
	for (const Ptr& child : children)
	{

		if (child)
		{
			child->parent = nullptr;
		}
	}
	children.clear();
	dirty = true;
}


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


UIElement::Ptr UIElement::FindById(std::string_view wantedId)
{
	if (id == wantedId)
	{
		return shared_from_this();
	}


	for (const Ptr& child : children)
	{
		if (!child)
		{
			continue;
		}

		Ptr found = child->FindById(wantedId);

		if (found)
		{
			return found;
		}
	}

	return nullptr;
}


bool UIElement::HasClass(std::string_view className) const
{
	return std::find(classList.begin(), classList.end(), className) != classList.end();
}


bool UIElement::IsAncestorOf(const UIElement* candidate) const
{
	const UIElement* current = candidate;

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


std::string UIElement::GetDisplayLabel() const
{
	if (!id.empty())
	{
		return typeName + " [" + id + "]";
	}


	if (!text.empty())
	{
		return typeName + " \"" + text + "\"";
	}

	return typeName;
}


void UIElement::SetClassListFromString(std::string_view classText)
{
	classList.clear();

	std::stringstream stream{ std::string(classText) };
	std::string token;

	while (stream >> token)
	{
		classList.push_back(token);
	}

	dirty = true;
}


std::string UIElement::GetClassListAsString() const
{
	std::string joined;

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


void UIElement::MarkDirty()
{
	dirty = true;

	UIElement* current = parent;

	while (current != nullptr)
	{
		current->dirty = true;
		current = current->parent;
	}
}


void UIElement::ClearDirtyRecursive()
{
	dirty = false;

	for (const Ptr& child : children)
	{
		if (child)
		{
			child->ClearDirtyRecursive();
		}
	}
}


void UIElement::SetVisible(bool nextVisible)
{
	if (visible == nextVisible)
	{
		return;
	}

	visible = nextVisible;
	MarkDirty();
}


bool UIElement::IsVisible() const
{
	return visible;
}


void UIElement::SetHoverState(bool hovered)
{
	hoverActive_ = hovered;
}


bool UIElement::AdvanceAnimation(float deltaTime)
{
	const float previousBlend = hoverBlend_;
	const float targetBlend = hoverActive_ ? 1.0f : 0.0f;


	if (hoverBlend_ < targetBlend)
	{
		hoverBlend_ = std::min(targetBlend, hoverBlend_ + (deltaTime * kHoverAnimationSpeed));
	}

	else if (hoverBlend_ > targetBlend)
	{
		hoverBlend_ = std::max(targetBlend, hoverBlend_ - (deltaTime * kHoverAnimationSpeed));
	}


	if (std::abs(hoverBlend_ - previousBlend) > 0.0001f)
	{
		RefreshDisplayStyle();
		return true;
	}

	return false;
}


bool UIElement::AdvanceAnimationRecursive(float deltaTime)
{
	bool changed = AdvanceAnimation(deltaTime);

	for (const Ptr& child : children)
	{
		if (child)
		{
			changed = child->AdvanceAnimationRecursive(deltaTime) || changed;
		}
	}
	return changed;
}


UIStyle UIElement::BuildTargetDisplayStyle() const
{
	UIStyle target = computedStyle;
	target.MergeFrom(hoverStyle);
	return target;
}


void UIElement::RefreshDisplayStyle()
{
	displayStyle = UIStyle::Lerp(computedStyle, BuildTargetDisplayStyle(), hoverBlend_);
}
