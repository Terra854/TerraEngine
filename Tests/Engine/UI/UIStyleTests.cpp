#include "TestHarness.h"

#include "UI/UIStyle.h"

#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
			return std::fabs(left - right) <= epsilon;
		}
	}

	void RegisterUIStyleTests(TestRegistry& registry) {
		registry.Add("Engine.UIStyle.ParseUIColorSupportsHexAndRgba", [](TestContext& context) {
			const std::optional<glm::vec4> hexColor = ParseUIColor("#3366cc80");
			const std::optional<glm::vec4> rgbaColor = ParseUIColor("rgba(255, 128, 0, 0.25)");

			context.Expect(hexColor.has_value(), "Eight-digit hex colors should parse with alpha.");
			context.Expect(NearlyEqual(hexColor->x, 0.2f), "Hex red should decode to normalized float color.");
			context.Expect(NearlyEqual(hexColor->y, 0.4f), "Hex green should decode to normalized float color.");
			context.Expect(NearlyEqual(hexColor->z, 0.8f), "Hex blue should decode to normalized float color.");
			context.Expect(NearlyEqual(hexColor->w, 128.0f / 255.0f), "Hex alpha should decode to normalized float color.");

			context.Expect(rgbaColor.has_value(), "rgba colors should parse four comma-separated channels.");
			context.Expect(NearlyEqual(rgbaColor->x, 1.0f), "rgba red should normalize from 255.");
			context.Expect(NearlyEqual(rgbaColor->y, 128.0f / 255.0f), "rgba green should normalize from 128.");
			context.Expect(NearlyEqual(rgbaColor->z, 0.0f), "rgba blue should normalize from 0.");
			context.Expect(NearlyEqual(rgbaColor->w, 0.25f), "rgba alpha should remain in authored 0-1 space.");

			context.Expect(!ParseUIColor("#nothex").has_value(), "Invalid hex colors should be rejected.");
			context.Expect(!ParseUIColor("rgb(1, 2, 3)").has_value(), "Unsupported color functions should be rejected.");
		});

		registry.Add("Engine.UIStyle.ParseUIValueSupportsAutoPointsAndPercent", [](TestContext& context) {
			const std::optional<UIValue> autoValue = ParseUIValue("auto");
			const std::optional<UIValue> pointValue = ParseUIValue("24px");
			const std::optional<UIValue> barePointValue = ParseUIValue("12.5");
			const std::optional<UIValue> percentValue = ParseUIValue("75%");

			context.Expect(autoValue.has_value() && autoValue->IsAuto(), "The auto keyword should parse as an auto UI value.");
			context.Expect(pointValue.has_value() && pointValue->unit == UIValue::Unit::Points && NearlyEqual(pointValue->value, 24.0f), "Pixel values should parse as point values.");
			context.Expect(barePointValue.has_value() && barePointValue->unit == UIValue::Unit::Points && NearlyEqual(barePointValue->value, 12.5f), "Bare numbers should parse as point values.");
			context.Expect(percentValue.has_value() && percentValue->unit == UIValue::Unit::Percent && NearlyEqual(percentValue->value, 75.0f), "Percent values should keep the authored percent number.");
			context.Expect(!ParseUIValue("twenty").has_value(), "Invalid numeric values should be rejected.");
		});

		registry.Add("Engine.UIStyle.ParseUIEdgeValuesExpandsCssShorthands", [](TestContext& context) {
			const std::optional<UIEdgeValues> oneValue = ParseUIEdgeValues("4px");
			const std::optional<UIEdgeValues> twoValues = ParseUIEdgeValues("4px 8px");
			const std::optional<UIEdgeValues> threeValues = ParseUIEdgeValues("1px 2px 3px");
			const std::optional<UIEdgeValues> fourValues = ParseUIEdgeValues("1px 2px 3px 4px");

			context.Expect(oneValue.has_value(), "A one-value edge shorthand should parse.");
			context.Expect(NearlyEqual(oneValue->top.value, 4.0f) && NearlyEqual(oneValue->right.value, 4.0f) && NearlyEqual(oneValue->bottom.value, 4.0f) && NearlyEqual(oneValue->left.value, 4.0f), "One-value shorthand should apply to every edge.");

			context.Expect(twoValues.has_value(), "A two-value edge shorthand should parse.");
			context.Expect(NearlyEqual(twoValues->top.value, 4.0f) && NearlyEqual(twoValues->bottom.value, 4.0f), "Two-value shorthand should apply the first value vertically.");
			context.Expect(NearlyEqual(twoValues->right.value, 8.0f) && NearlyEqual(twoValues->left.value, 8.0f), "Two-value shorthand should apply the second value horizontally.");

			context.Expect(threeValues.has_value(), "A three-value edge shorthand should parse.");
			context.Expect(NearlyEqual(threeValues->top.value, 1.0f) && NearlyEqual(threeValues->right.value, 2.0f) && NearlyEqual(threeValues->bottom.value, 3.0f) && NearlyEqual(threeValues->left.value, 2.0f), "Three-value shorthand should mirror the horizontal edge.");

			context.Expect(fourValues.has_value(), "A four-value edge shorthand should parse.");
			context.Expect(NearlyEqual(fourValues->top.value, 1.0f) && NearlyEqual(fourValues->right.value, 2.0f) && NearlyEqual(fourValues->bottom.value, 3.0f) && NearlyEqual(fourValues->left.value, 4.0f), "Four-value shorthand should map top, right, bottom, left in order.");
			context.Expect(!ParseUIEdgeValues("1px 2px 3px 4px 5px").has_value(), "Five-value edge shorthands should be rejected.");
		});

		registry.Add("Engine.UIStyle.MergeAndLerpPreserveOptionalStyleIntent", [](TestContext& context) {
			UIStyle baseStyle{};
			baseStyle.opacity = 0.25f;
			baseStyle.transform.scale = 1.0f;

			UIStyle overrideStyle{};
			overrideStyle.color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			overrideStyle.opacity = 0.75f;

			baseStyle.MergeFrom(overrideStyle);
			context.Expect(baseStyle.color.has_value(), "MergeFrom should copy set optional color properties.");
			context.Expect(baseStyle.opacity.has_value() && NearlyEqual(*baseStyle.opacity, 0.75f), "MergeFrom should override set optional scalar properties.");
			context.Expect(baseStyle.transform.scale.has_value() && NearlyEqual(*baseStyle.transform.scale, 1.0f), "MergeFrom should leave destination properties unchanged when the source does not set them.");

			UIStyle targetStyle{};
			targetStyle.opacity = 1.0f;
			targetStyle.transform.scale = 2.0f;
			targetStyle.backgroundColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

			const UIStyle halfway = UIStyle::Lerp(baseStyle, targetStyle, 0.5f);
			context.Expect(halfway.opacity.has_value() && NearlyEqual(*halfway.opacity, 0.875f), "Lerp should interpolate scalar properties present on both styles.");
			context.Expect(halfway.transform.scale.has_value() && NearlyEqual(*halfway.transform.scale, 1.5f), "Lerp should interpolate transform scale properties present on both styles.");
			context.Expect(halfway.backgroundColor.has_value(), "Lerp should adopt target properties that were absent from the source style.");
		});
	}
}
