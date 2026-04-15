#pragma once

#include "UIElement.h"

#include <glm/vec2.hpp>
#include <yoga/Yoga.h>

#include <optional>

// UILayoutEngine is a thin wrapper around Yoga that translates TerraEngine's
// CSS-like authored styles into concrete box positions and sizes.
class UILayoutEngine
{
public:
	void ComputeLayout(const UIElement::Ptr& root, const glm::vec2& viewportSize) const;

private:
	void ApplyStyleToNode(const UIElement& element, YGNodeRef node) const;
	YGNodeRef BuildNodeTree(const UIElement::Ptr& element) const;
	void SynchronizeLayout(const UIElement::Ptr& element, const glm::vec2& parentOrigin) const;

	void ApplyMeasuredValue(
		const std::optional<UIValue>& value,
		YGNodeRef node,
		void (*setPoints)(YGNodeRef, float),
		void (*setPercent)(YGNodeRef, float),
		void (*setAuto)(YGNodeRef) = nullptr) const;

	void ApplyEdgeValues(
		const std::optional<UIEdgeValues>& values,
		YGNodeRef node,
		void (*setPoints)(YGNodeRef, YGEdge, float),
		void (*setPercent)(YGNodeRef, YGEdge, float),
		void (*setAuto)(YGNodeRef, YGEdge) = nullptr) const;
};
