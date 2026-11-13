#include "TestHarness.h"
#include "UI/UIParser.h"
#include <string>

namespace TerraEngine::Tests {
	void RegisterMarkupReferenceRoundTripTests(TestRegistry& registry) {
		registry.Add("Engine.UIParser.AttributeEntitiesSurviveSerialization", [](TestContext& context) {
			const UIParser parser;
			// Parse entities first, so the serializer must re-escape decoded values.
			// This checks actual authoring round-trip behavior, not source spelling.
			const auto before = parser.ParseMarkupString(
				R"(<UIRoot id="root&amp;&lt;&gt;&quot;" class="root&amp;&lt;&gt;&quot;">
	<Label id="label&amp;&lt;&gt;&quot;" class="one&amp;&lt;&gt;&quot; second" text="Say &quot;Hi&quot; &amp; &lt;tag&gt;" src="image&amp;&lt;&gt;&quot;.png" style="font-family: Example&quot;Face&amp;&lt;&gt;;" />
</UIRoot>)", "EntityRoundTripInput");
			context.Expect(before && before->children.size() == 1U, "Input fixture must contain one label.");
			const auto encoded = parser.SerializeMarkup(before);
			const auto after = parser.ParseMarkupString(encoded, "EntityRoundTripOutput");
			context.Expect(after && after->children.size() == 1U, "Serialized markup must remain a valid tree.");
			context.ExpectEqual(before->id, after->id, "Root identity must survive decoded XML entities.");
			context.Expect(before->classList == after->classList, "Root classes must round-trip.");
			const auto& left = before->children.front();
			const auto& right = after->children.front();
			context.Expect(left && right, "The label must remain present.");
			context.ExpectEqual(left->id, right->id, "Label identity must round-trip.");
			context.Expect(left->classList == right->classList, "Label classes must round-trip.");
			context.ExpectEqual(std::string("Say \"Hi\" & <tag>"), left->text, "The input must contain decoded special characters.");
			context.ExpectEqual(left->text, right->text, "Text must round-trip.");
			context.ExpectEqual(left->source, right->source, "Source references must round-trip.");
			context.Expect(left->inlineStyle.fontFamily.has_value(), "The input must exercise inline font-family.");
			context.Expect(left->inlineStyle.fontFamily == right->inlineStyle.fontFamily, "Inline style attributes must round-trip.");
		});

		registry.Add("Engine.UIParser.SingleAndMultipleStyleReferencesSurviveEntities", [](TestContext& context) {
			const UIParser parser;
			for (const std::string attribute : {
				std::string(R"(styleSrc="shared&amp;&lt;&gt;&quot;.tuss")"),
				std::string(R"(styleSrcs="shared&amp;&lt;&gt;&quot;.tuss;other&amp;&lt;&gt;&quot;.tuss")") }) {
				const auto before = parser.ParseMarkupString("<UIRoot " + attribute + " />", "StyleReferenceInput");
				context.Expect(before && !before->styleSheetSources.empty(), "The fixture must exercise a real stylesheet reference.");
				const auto after = parser.ParseMarkupString(parser.SerializeMarkup(before), "StyleReferenceOutput");
				context.Expect(after != nullptr, "Serialized references must remain valid markup.");
				context.Expect(before->styleSheetSources == after->styleSheetSources, "Every decoded stylesheet path and its order must round-trip.");
			}
		});
	}
}

