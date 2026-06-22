// UILayoutEngine.h — Wrapper for the Yoga layout engine integration.
//
// This file defines the UILayoutEngine class, which acts as a bridge between the
// engine's UIElement tree and the Facebook Yoga flexbox library. It manages
// Yoga node lifecycles and provides high-level functions to compute the size
// and position of all elements in a UI hierarchy.

#pragma once

#include "UIElement.h"

#include <glm/vec2.hpp>
#include <yoga/Yoga.h>

#include <optional>

// UILayoutEngine is a thin wrapper around Yoga that translates TerraEngine's
// CSS-like authored styles into concrete box positions and sizes. Layout work
// can run on cloned preparation trees, so each call owns its temporary Yoga
// nodes from BuildNodeTree through ComputeLayout and clears them before return.
// Never share these nodes with the live UI tree or another worker task.
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
