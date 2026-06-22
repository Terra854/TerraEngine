#include "TestHarness.h"

#include "UI/UIElement.h"
#include "UI/UISystem.h"

#include <cmath>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
			return std::fabs(left - right) <= epsilon;
		}
	}

	void RegisterUIElementTests(TestRegistry& registry) {
		registry.Add("Engine.UIElement.ChildOperationsMaintainParentLinksAndDirtyState", [](TestContext& context) {
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr first = UIElement::Create(UIElementType::Label);
			const UIElement::Ptr second = UIElement::Create(UIElementType::Button);
			first->id = "first";
			second->id = "second";

			root->ClearDirtyRecursive();
			context.Expect(root->InsertChild(5, first), "InsertChild should clamp out-of-range indices and insert the child.");
			root->AddChild(second);
			context.ExpectEqual(2u, static_cast<unsigned int>(root->children.size()), "The root should own both inserted children.");
			context.Expect(first->parent == root.get() && second->parent == root.get(), "Inserted children should point back at their parent.");
			context.Expect(root->dirty, "Adding children should mark the parent dirty.");

			root->ClearDirtyRecursive();
			context.Expect(root->MoveChild(1, 0), "MoveChild should reorder valid child indices.");
			context.Expect(root->children[0] == second && root->children[1] == first, "MoveChild should reorder the child vector.");
			context.Expect(root->dirty, "Moving children should mark the parent dirty.");

			root->ClearDirtyRecursive();
			context.Expect(root->RemoveChild(first), "RemoveChild should detach an existing child.");
			context.Expect(first->parent == nullptr, "Removed children should clear their parent pointer.");
			context.ExpectEqual(1u, static_cast<unsigned int>(root->children.size()), "RemoveChild should erase the child from the parent vector.");
			context.Expect(root->dirty, "Removing children should mark the parent dirty.");
		});

		registry.Add("Engine.UIElement.ReparentPreventsCyclesAndMovesBetweenParents", [](TestContext& context) {
			const UIElement::Ptr leftParent = UIElement::Create(UIElementType::VisualElement);
			const UIElement::Ptr rightParent = UIElement::Create(UIElementType::VisualElement);
			const UIElement::Ptr child = UIElement::Create(UIElementType::Button);
			const UIElement::Ptr grandchild = UIElement::Create(UIElementType::Label);

			leftParent->AddChild(child);
			child->AddChild(grandchild);

			context.Expect(!leftParent->ReparentTo(grandchild), "Reparenting an ancestor under its descendant should be rejected.");
			context.Expect(leftParent->parent == nullptr, "Rejected cycle reparenting should leave the ancestor parent unchanged.");

			context.Expect(child->ReparentTo(rightParent), "ReparentTo should move a child between valid parents.");
			context.Expect(child->parent == rightParent.get(), "Reparented children should point at the new parent.");
			context.Expect(leftParent->children.empty(), "The old parent should no longer own the reparented child.");
			context.ExpectEqual(1u, static_cast<unsigned int>(rightParent->children.size()), "The new parent should own the reparented child.");
			context.Expect(rightParent->children[0] == child, "The moved child should be inserted into the new parent.");
		});

		registry.Add("Engine.UIElement.ClassListFindByIdAndDisplayLabelsStayStable", [](TestContext& context) {
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr panel = UIElement::Create(UIElementType::VisualElement);
			const UIElement::Ptr label = UIElement::CreateDefault(UIElementType::Label);

			panel->id = "panel";
			panel->SetClassListFromString("primary selected");
			label->id = "title";
			label->text = "Hello";
			root->AddChild(panel);
			panel->AddChild(label);

			context.Expect(panel->HasClass("primary") && panel->HasClass("selected"), "Class list parsing should split whitespace-delimited classes.");
			context.ExpectEqual(std::string("primary selected"), panel->GetClassListAsString(), "Class list serialization should preserve authored order.");
			context.Expect(root->FindById("title") == label, "FindById should search descendants recursively.");
			context.Expect(root->IsAncestorOf(label.get()), "IsAncestorOf should recognize nested descendants.");
			context.ExpectEqual(std::string("VisualElement [panel]"), panel->GetDisplayLabel(), "Display labels should prefer element ids.");
			context.ExpectEqual(std::string("Label [title]"), label->GetDisplayLabel(), "Display labels should include ids before text labels.");
		});

		registry.Add("Engine.UIElement.CloneCopiesSubtreeWithoutSharingParentPointers", [](TestContext& context) {
			const UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			const UIElement::Ptr button = UIElement::CreateDefault(UIElementType::Button);
			button->id = "start";
			button->SetClassListFromString("cta");
			button->inlineStyle.opacity = 0.5f;
			button->computedStyle.opacity = 0.25f;
			root->AddChild(button);

			const UIElement::Ptr clone = root->Clone();

			context.Expect(clone != root, "Clone should return a distinct root element.");
			context.ExpectEqual(1u, static_cast<unsigned int>(clone->children.size()), "Clone should copy the child subtree.");
			context.Expect(clone->children[0] != button, "Clone should deep-copy child elements.");
			context.Expect(clone->children[0]->parent == clone.get(), "Cloned children should point at the cloned parent.");
			context.ExpectEqual(std::string("start"), clone->children[0]->id, "Clone should preserve child ids.");
			context.Expect(clone->children[0]->HasClass("cta"), "Clone should preserve child classes.");
			context.Expect(clone->children[0]->inlineStyle.opacity.has_value() && NearlyEqual(*clone->children[0]->inlineStyle.opacity, 0.5f), "Clone should preserve inline style values.");
			context.Expect(clone->children[0]->computedStyle.opacity.has_value() && NearlyEqual(*clone->children[0]->computedStyle.opacity, 0.25f), "Clone should preserve computed style values.");
			context.ExpectEqual(
				static_cast<unsigned int>(UISystem::CountElementTree(root)),
				static_cast<unsigned int>(UISystem::CountElementTree(clone)),
				"Worker-safe cloned trees should preserve deterministic preparation counts.");
		});

		registry.Add("Engine.UIElement.VisibilityAndHoverAnimationUpdateDisplayStyle", [](TestContext& context) {
			const UIElement::Ptr button = UIElement::Create(UIElementType::Button);
			button->computedStyle.opacity = 0.25f;
			button->hoverStyle.opacity = 1.0f;
			button->RefreshDisplayStyle();

			button->ClearDirtyRecursive();
			button->SetVisible(false);
			context.Expect(!button->IsVisible(), "SetVisible should update element visibility.");
			context.Expect(button->dirty, "Changing visibility should mark the element dirty.");

			button->SetHoverState(true);
			const bool changed = button->AdvanceAnimation(0.1f);
			context.Expect(changed, "Advancing toward an active hover state should report an animation change.");
			context.Expect(button->displayStyle.opacity.has_value(), "Hover animation should refresh display style.");
			context.Expect(*button->displayStyle.opacity > 0.25f && *button->displayStyle.opacity < 1.0f, "Hover animation should blend from computed style toward hover style.");
		});
	}
}
