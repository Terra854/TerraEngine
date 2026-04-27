// UILayoutEngine.cpp — Implementation of the Yoga-based UI layout system.
//
// This file implements the translation of UIStyle properties into Yoga-specific
// flexbox attributes. It handles the recursive construction of the Yoga node
// tree, the execution of the layout pass, and the synchronization of the
// resulting coordinates back into the UIElement instances.

#include "UILayoutEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
// ToYogaFlexDirection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
YGFlexDirection ToYogaFlexDirection(UIFlexDirection direction)
{
	return direction == UIFlexDirection::Row ? YGFlexDirectionRow : YGFlexDirectionColumn;
}

// ToYogaJustify is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
YGJustify ToYogaJustify(UIJustifyContent justifyContent)
{
	switch (justifyContent)
	{
	case UIJustifyContent::Center:
		return YGJustifyCenter;
	case UIJustifyContent::FlexEnd:
		return YGJustifyFlexEnd;
	case UIJustifyContent::SpaceBetween:
		return YGJustifySpaceBetween;
	case UIJustifyContent::SpaceAround:
		return YGJustifySpaceAround;
	case UIJustifyContent::FlexStart:
	default:
		return YGJustifyFlexStart;
	}
}

// ToYogaAlign is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
YGAlign ToYogaAlign(UIAlignItems alignItems)
{
	switch (alignItems)
	{
	case UIAlignItems::FlexStart:
		return YGAlignFlexStart;
	case UIAlignItems::Center:
		return YGAlignCenter;
	case UIAlignItems::FlexEnd:
		return YGAlignFlexEnd;
	case UIAlignItems::Stretch:
	default:
		return YGAlignStretch;
	}
}

// EstimateIntrinsicTextWidth is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
float EstimateIntrinsicTextWidth(const UIElement& element)
{
	const float fontSize = element.displayStyle.fontSize.value_or(18.0f);
	return std::max(32.0f, static_cast<float>(element.text.size()) * fontSize * 0.58f);
}
}

// UILayoutEngine::ComputeLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UILayoutEngine::ComputeLayout(const UIElement::Ptr& root, const glm::vec2& viewportSize) const
{
	if (!root)
	{
		return;
	}

	YGNodeRef yogaRoot = BuildNodeTree(root);
	
	// Force the root element to always occupy the full viewport dimensions
	// regardless of its authored style. This ensures percentage-based children
	// resolve correctly and the UI shell fills the intended interaction area.
	YGNodeStyleSetWidth(yogaRoot, viewportSize.x);
	YGNodeStyleSetHeight(yogaRoot, viewportSize.y);

	YGNodeCalculateLayout(yogaRoot, viewportSize.x, viewportSize.y, YGDirectionLTR);
	SynchronizeLayout(root, glm::vec2(0.0f, 0.0f));
	YGNodeFreeRecursive(yogaRoot);

	const auto clearNodePointers = [](const auto& self, const UIElement::Ptr& element) -> void {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!element)
		{
			return;
		}

		element->yogaNode = nullptr;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const UIElement::Ptr& child : element->children)
		{
			self(self, child);
		}
	};
	clearNodePointers(clearNodePointers, root);
}

// UILayoutEngine::ApplyStyleToNode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UILayoutEngine::ApplyStyleToNode(const UIElement& element, YGNodeRef node) const
{
	const UIStyle& style = element.computedStyle;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexDirection.has_value())
	{
		YGNodeStyleSetFlexDirection(node, ToYogaFlexDirection(*style.flexDirection));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.justifyContent.has_value())
	{
		YGNodeStyleSetJustifyContent(node, ToYogaJustify(*style.justifyContent));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.alignItems.has_value())
	{
		YGNodeStyleSetAlignItems(node, ToYogaAlign(*style.alignItems));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexGrow.has_value())
	{
		YGNodeStyleSetFlexGrow(node, *style.flexGrow);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.flexShrink.has_value())
	{
		YGNodeStyleSetFlexShrink(node, *style.flexShrink);
	}

	ApplyMeasuredValue(style.flexBasis, node, &YGNodeStyleSetFlexBasis, &YGNodeStyleSetFlexBasisPercent, &YGNodeStyleSetFlexBasisAuto);
	ApplyMeasuredValue(style.width, node, &YGNodeStyleSetWidth, &YGNodeStyleSetWidthPercent, &YGNodeStyleSetWidthAuto);
	ApplyMeasuredValue(style.height, node, &YGNodeStyleSetHeight, &YGNodeStyleSetHeightPercent, &YGNodeStyleSetHeightAuto);
	ApplyEdgeValues(style.margin, node, &YGNodeStyleSetMargin, &YGNodeStyleSetMarginPercent, &YGNodeStyleSetMarginAuto);
	ApplyEdgeValues(style.padding, node, &YGNodeStyleSetPadding, &YGNodeStyleSetPaddingPercent, nullptr);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (style.borderWidth.has_value())
	{
		YGNodeStyleSetBorder(node, YGEdgeAll, *style.borderWidth);
	}

	// Simple intrinsic sizing gives text-driven controls a useful footprint even
	// before dedicated Yoga measure callbacks are introduced.
	if (!style.width.has_value() && (element.type == UIElementType::Label || element.type == UIElementType::Button) && !element.text.empty())
	{
		YGNodeStyleSetWidth(node, EstimateIntrinsicTextWidth(element));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!style.height.has_value() && (element.type == UIElementType::Label || element.type == UIElementType::Button))
	{
		YGNodeStyleSetHeight(node, element.displayStyle.fontSize.value_or(18.0f) + 16.0f);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!style.width.has_value() && element.type == UIElementType::Image)
	{
		YGNodeStyleSetWidth(node, 100.0f);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!style.height.has_value() && element.type == UIElementType::Image)
	{
		YGNodeStyleSetHeight(node, 100.0f);
	}
}

// UILayoutEngine::BuildNodeTree is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
YGNodeRef UILayoutEngine::BuildNodeTree(const UIElement::Ptr& element) const
{
	YGNodeRef node = YGNodeNew();
	element->yogaNode = node;
	ApplyStyleToNode(*element, node);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t childIndex = 0; childIndex < element->children.size(); ++childIndex)
	{
		const UIElement::Ptr& child = element->children[childIndex];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!child)
		{
			continue;
		}

		YGNodeRef childNode = BuildNodeTree(child);
		YGNodeInsertChild(node, childNode, childIndex);
	}

	return node;
}

// UILayoutEngine::SynchronizeLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UILayoutEngine::SynchronizeLayout(const UIElement::Ptr& element, const glm::vec2& parentOrigin) const
{
	if (!element || element->yogaNode == nullptr)
	{
		return;
	}

	YGNodeRef node = static_cast<YGNodeRef>(element->yogaNode);
	const glm::vec2 localPosition(YGNodeLayoutGetLeft(node), YGNodeLayoutGetTop(node));
	element->calculatedPosition = parentOrigin + localPosition;
	element->calculatedSize = glm::vec2(YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node));

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const UIElement::Ptr& child : element->children)
	{
		SynchronizeLayout(child, element->calculatedPosition);
	}
}

void UILayoutEngine::ApplyMeasuredValue(
	const std::optional<UIValue>& value,
	YGNodeRef node,
	void (*setPoints)(YGNodeRef, float),
	void (*setPercent)(YGNodeRef, float),
	void (*setAuto)(YGNodeRef)) const
{
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!value.has_value())
	{
		return;
	}

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (value->unit)
	{
	case UIValue::Unit::Points:
		setPoints(node, value->value);
		break;
	case UIValue::Unit::Percent:
		setPercent(node, value->value);
		break;
	case UIValue::Unit::Auto:
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (setAuto != nullptr)
		{
			setAuto(node);
		}
		break;
	}
}

void UILayoutEngine::ApplyEdgeValues(
	const std::optional<UIEdgeValues>& values,
	YGNodeRef node,
	void (*setPoints)(YGNodeRef, YGEdge, float),
	void (*setPercent)(YGNodeRef, YGEdge, float),
	void (*setAuto)(YGNodeRef, YGEdge)) const
{
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!values.has_value())
	{
		return;
	}

	const auto applySingleValue = [&](YGEdge edge, const UIValue& value) {
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (value.unit)
		{
		case UIValue::Unit::Points:
			setPoints(node, edge, value.value);
			break;
		case UIValue::Unit::Percent:
			setPercent(node, edge, value.value);
			break;
		case UIValue::Unit::Auto:
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setAuto != nullptr)
			{
				setAuto(node, edge);
			}
			break;
		}
	};

	applySingleValue(YGEdgeTop, values->top);
	applySingleValue(YGEdgeRight, values->right);
	applySingleValue(YGEdgeBottom, values->bottom);
	applySingleValue(YGEdgeLeft, values->left);
}
