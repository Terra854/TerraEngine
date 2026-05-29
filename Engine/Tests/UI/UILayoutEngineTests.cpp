#include "TestHarness.h"

#include "UI/UILayoutEngine.h"

#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
			return std::fabs(left - right) <= epsilon;
		}
	}

	void RegisterUILayoutEngineTests(TestRegistry& registry) {
		registry.Add("Engine.UILayoutEngine.RootFillsViewportAndClearsYogaPointers", [](TestContext& context) {
			const UILayoutEngine layoutEngine;
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr child = UIElement::Create(UIElementType::VisualElement);
			child->computedStyle.width = UIValue::Points(64.0f);
			child->computedStyle.height = UIValue::Points(32.0f);
			root->AddChild(child);

			layoutEngine.ComputeLayout(root, glm::vec2(800.0f, 600.0f));

			context.Expect(NearlyEqual(root->calculatedSize.x, 800.0f) && NearlyEqual(root->calculatedSize.y, 600.0f), "The layout root should fill the requested viewport.");
			context.Expect(NearlyEqual(child->calculatedSize.x, 64.0f) && NearlyEqual(child->calculatedSize.y, 32.0f), "Fixed-size children should keep their authored dimensions.");
			context.Expect(root->yogaNode == nullptr && child->yogaNode == nullptr, "Temporary Yoga nodes should be cleared after layout synchronization.");
		});

		registry.Add("Engine.UILayoutEngine.RowFlexPlacesChildrenInOrder", [](TestContext& context) {
			const UILayoutEngine layoutEngine;
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr first = UIElement::Create(UIElementType::VisualElement);
			const UIElement::Ptr second = UIElement::Create(UIElementType::VisualElement);
			root->computedStyle.flexDirection = UIFlexDirection::Row;
			first->computedStyle.width = UIValue::Points(100.0f);
			first->computedStyle.height = UIValue::Points(40.0f);
			second->computedStyle.width = UIValue::Points(150.0f);
			second->computedStyle.height = UIValue::Points(40.0f);
			root->AddChild(first);
			root->AddChild(second);

			layoutEngine.ComputeLayout(root, glm::vec2(500.0f, 100.0f));

			context.Expect(NearlyEqual(first->calculatedPosition.x, 0.0f), "The first row child should start at the left edge.");
			context.Expect(NearlyEqual(second->calculatedPosition.x, 100.0f), "The second row child should be placed after the first child width.");
			context.Expect(NearlyEqual(second->calculatedSize.x, 150.0f), "The second row child should keep its authored width.");
		});

		registry.Add("Engine.UILayoutEngine.PercentSizesResolveAgainstParent", [](TestContext& context) {
			const UILayoutEngine layoutEngine;
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr child = UIElement::Create(UIElementType::VisualElement);
			child->computedStyle.width = UIValue::Percent(50.0f);
			child->computedStyle.height = UIValue::Percent(25.0f);
			root->AddChild(child);

			layoutEngine.ComputeLayout(root, glm::vec2(400.0f, 200.0f));

			context.Expect(NearlyEqual(child->calculatedSize.x, 200.0f), "Percentage width should resolve against the parent width.");
			context.Expect(NearlyEqual(child->calculatedSize.y, 50.0f), "Percentage height should resolve against the parent height.");
		});

		registry.Add("Engine.UILayoutEngine.IntrinsicLabelAndImageFallbackSizesApply", [](TestContext& context) {
			const UILayoutEngine layoutEngine;
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr label = UIElement::Create(UIElementType::Label);
			const UIElement::Ptr image = UIElement::Create(UIElementType::Image);
			root->computedStyle.alignItems = UIAlignItems::FlexStart;
			label->text = "Hello";
			label->displayStyle.fontSize = 20.0f;
			root->AddChild(label);
			root->AddChild(image);

			layoutEngine.ComputeLayout(root, glm::vec2(300.0f, 300.0f));

			context.Expect(NearlyEqual(label->calculatedSize.x, 58.0f), "Text elements without authored width should receive an intrinsic text width.");
			context.Expect(NearlyEqual(label->calculatedSize.y, 36.0f), "Text elements without authored height should receive an intrinsic text height.");
			context.Expect(NearlyEqual(image->calculatedSize.x, 100.0f) && NearlyEqual(image->calculatedSize.y, 100.0f), "Images without authored dimensions should receive the fallback image size.");
		});
	}
}
