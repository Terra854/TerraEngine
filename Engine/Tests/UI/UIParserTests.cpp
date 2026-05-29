#include "TestHarness.h"

#include "UI/UIParser.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
			return std::fabs(left - right) <= epsilon;
		}

		bool ContainsText(const std::string& haystack, const std::string& needle) {
			return haystack.find(needle) != std::string::npos;
		}

		// Parser error tests care about the diagnostic text because callers use
		// debug names to identify which inline document failed to load.
		bool ThrowsMessageContaining(const std::function<void()>& operation, const std::string& expectedText) {
			try {
				operation();
			} catch(const std::runtime_error& error) {
				return ContainsText(error.what(), expectedText);
			}
			return false;
		}

		// File API tests write tiny documents to a deterministic temp directory so
		// they exercise ResourceManager-backed reads without requiring fixtures.
		std::filesystem::path MakeParserTempDirectory() {
			const std::filesystem::path directory = std::filesystem::temp_directory_path() / "TerraEngine_UIParserFileTests";
			std::filesystem::remove_all(directory);
			std::filesystem::create_directories(directory);
			return directory;
		}

		void WriteTextFile(const std::filesystem::path& path, const std::string& contents) {
			std::ofstream stream(path, std::ios::binary);
			stream << contents;
		}
	}

	void RegisterUIParserTests(TestRegistry& registry) {
		registry.Add("Engine.UIParser.ParseMarkupStringBuildsTypedTree", [](TestContext& context) {
			const UIParser parser;
			const UIElement::Ptr root = parser.ParseMarkupString(
R"(<VisualElement id="panel" class="primary wide" styleSrcs="main.tuss; theme.tuss" style="width: 320px; padding: 4px 8px;">
	<Label id="title" text="Launch" />
	<Button id="start" class="cta" text="Start" />
</VisualElement>)",
				"InlineTree");

			context.Expect(root != nullptr, "Parsing valid markup should produce a root element.");
			context.Expect(root->type == UIElementType::Root, "Non-root markup should be wrapped in a UIRoot element.");
			context.ExpectEqual(1u, static_cast<unsigned int>(root->children.size()), "The wrapper root should own the authored element.");

			const UIElement::Ptr panel = root->children[0];
			context.Expect(panel->type == UIElementType::VisualElement, "The authored VisualElement tag should map to a VisualElement node.");
			context.ExpectEqual(std::string("panel"), panel->id, "The id attribute should populate the element id.");
			context.Expect(panel->HasClass("primary") && panel->HasClass("wide"), "The class attribute should populate the class list.");
			context.ExpectEqual(2u, static_cast<unsigned int>(panel->styleSheetSources.size()), "Semicolon-delimited style sources should be parsed.");
			context.Expect(panel->inlineStyle.width.has_value(), "Inline width should be parsed into style data.");
			context.Expect(panel->inlineStyle.padding.has_value(), "Inline padding should be parsed into style data.");

			const UIElement::Ptr title = root->FindById("title");
			const UIElement::Ptr start = root->FindById("start");
			context.Expect(title != nullptr && title->type == UIElementType::Label, "Nested labels should be findable by id.");
			context.Expect(start != nullptr && start->type == UIElementType::Button, "Nested buttons should be findable by id.");
			context.Expect(title->parent == panel.get() && start->parent == panel.get(), "Parsed children should keep parent pointers.");
		});

		registry.Add("Engine.UIParser.ParseStyleStringCapturesSelectorsAndRecoverableErrors", [](TestContext& context) {
			const UIParser parser;
			const std::vector<UIStyleRule> rules = parser.ParseStyleString(
R"(:root { width: 100%; height: 100%; }
.cta { color: #00ff00; padding: 4px 8px; }
#start:hover { opacity: 0.5; scale: 1.25; }
.bad:active { opacity: 0.1; })");

			context.ExpectEqual(4u, static_cast<unsigned int>(rules.size()), "Each stylesheet block should produce one parsed rule.");
			context.Expect(rules[0].selectorKind == UISelectorKind::Root, "The :root selector should be parsed as a root selector.");
			context.Expect(rules[1].selectorKind == UISelectorKind::Class && rules[1].selectorValue == "cta", "Class selectors should preserve their selector value.");
			context.Expect(rules[1].style.color.has_value(), "Class rule color declarations should be parsed.");
			context.Expect(rules[1].style.padding.has_value(), "Class rule padding declarations should be parsed.");
			context.Expect(rules[2].selectorKind == UISelectorKind::Id && rules[2].pseudoState == UIPseudoState::Hover, "Id hover selectors should capture the pseudo state.");
			context.Expect(rules[2].style.transform.scale.has_value(), "Hover transform declarations should be parsed.");
			context.Expect(rules[3].validity == UISelectorValidityState::RecoverableError, "Unsupported pseudo states should be preserved as recoverable stylesheet errors.");
		});

		registry.Add("Engine.UIParser.ParseMarkupStringReportsMalformedMarkup", [](TestContext& context) {
			const UIParser parser;

			const bool rejectedMarkup = ThrowsMessageContaining([&parser]() {
				(void)parser.ParseMarkupString("<UIRoot><Label></UIRoot>", "BrokenMarkup");
			}, "BrokenMarkup");

			context.Expect(rejectedMarkup, "Malformed markup should throw an error that includes the caller-provided debug name.");
		});

		registry.Add("Engine.UIParser.ParseMarkupStringReportsEmptyMarkup", [](TestContext& context) {
			const UIParser parser;

			const bool rejectedMarkup = ThrowsMessageContaining([&parser]() {
				(void)parser.ParseMarkupString("   ", "EmptyMarkup");
			}, "EmptyMarkup");

			context.Expect(rejectedMarkup, "Empty markup should throw an error that includes the caller-provided debug name.");
		});

		registry.Add("Engine.UIParser.ParseStyleStringPreservesInvalidSelectorsAndValidDeclarations", [](TestContext& context) {
			const UIParser parser;
			const std::vector<UIStyleRule> rules = parser.ParseStyleString(".bad:active { opacity: 0.1; width: 12px; }");

			context.ExpectEqual(1u, static_cast<unsigned int>(rules.size()), "Unsupported pseudo-state blocks should still produce a style rule.");
			context.Expect(rules[0].validity == UISelectorValidityState::RecoverableError, "Unsupported pseudo-state selectors should be recoverable errors.");
			context.Expect(!rules[0].validationMessage.empty(), "Recoverable selector errors should carry a validation message.");
			context.Expect(rules[0].style.opacity.has_value() && NearlyEqual(*rules[0].style.opacity, 0.1f), "Recoverable selector rules should preserve valid opacity declarations.");
			context.Expect(rules[0].style.width.has_value() && NearlyEqual(rules[0].style.width->value, 12.0f), "Recoverable selector rules should preserve valid width declarations.");
		});

		registry.Add("Engine.UIParser.UnsupportedStyleDeclarationsDoNotCorruptValidProperties", [](TestContext& context) {
			const UIParser parser;
			const std::vector<UIStyleRule> rules = parser.ParseStyleString(".cta { unknown-property: nope; width: 64px; made-up: 10; opacity: 0.5; }");

			context.ExpectEqual(1u, static_cast<unsigned int>(rules.size()), "The parser should keep a rule that mixes unsupported and supported declarations.");
			context.Expect(rules[0].validity == UISelectorValidityState::Valid, "Unsupported declarations should not invalidate an otherwise valid selector.");
			context.Expect(rules[0].style.width.has_value() && NearlyEqual(rules[0].style.width->value, 64.0f), "Known declarations should still be parsed when unknown declarations are present.");
			context.Expect(rules[0].style.opacity.has_value() && NearlyEqual(*rules[0].style.opacity, 0.5f), "Known opacity should still be parsed when unknown declarations are present.");
		});

		registry.Add("Engine.UIParser.TryParseMarkupFileLoadsValidTempFile", [](TestContext& context) {
			const std::filesystem::path directory = MakeParserTempDirectory();
			const std::filesystem::path markupPath = directory / "valid.tuiml";
			WriteTextFile(markupPath, R"(<UIRoot><Label id="title" text="From disk" /></UIRoot>)");

			const UIParser parser;
			UIElement::Ptr root;
			std::string error = "stale error";
			const bool parsed = parser.TryParseMarkupFile(markupPath, root, error);

			context.Expect(parsed, "TryParseMarkupFile should return true for a valid markup file.");
			context.Expect(error.empty(), "TryParseMarkupFile should clear stale errors after a successful parse.");
			context.Expect(root != nullptr && root->FindById("title") != nullptr, "TryParseMarkupFile should populate the parsed root tree.");

			std::filesystem::remove_all(directory);
		});

		registry.Add("Engine.UIParser.TryParseStyleFileLoadsValidAndEmptyTempFiles", [](TestContext& context) {
			const std::filesystem::path directory = MakeParserTempDirectory();
			const std::filesystem::path stylePath = directory / "valid.tuss";
			const std::filesystem::path emptyStylePath = directory / "empty.tuss";
			WriteTextFile(stylePath, ".disk { width: 42px; opacity: 0.5; }");
			WriteTextFile(emptyStylePath, "");

			const UIParser parser;
			std::vector<UIStyleRule> rules;
			std::string error = "stale error";
			const bool parsed = parser.TryParseStyleFile(stylePath, rules, error);

			context.Expect(parsed, "TryParseStyleFile should return true for a valid stylesheet file.");
			context.Expect(error.empty(), "TryParseStyleFile should clear stale errors after a successful parse.");
			context.ExpectEqual(1u, static_cast<unsigned int>(rules.size()), "TryParseStyleFile should populate parsed style rules.");
			context.Expect(rules[0].style.width.has_value() && NearlyEqual(rules[0].style.width->value, 42.0f), "TryParseStyleFile should preserve parsed style values from disk.");

			rules.push_back(UIStyleRule{});
			error = "stale error";
			const bool parsedEmpty = parser.TryParseStyleFile(emptyStylePath, rules, error);
			context.Expect(parsedEmpty, "TryParseStyleFile should treat an existing empty stylesheet as valid.");
			context.Expect(error.empty(), "TryParseStyleFile should clear stale errors for empty stylesheet files.");
			context.Expect(rules.empty(), "TryParseStyleFile should return no rules for an empty stylesheet.");

			std::filesystem::remove_all(directory);
		});

		registry.Add("Engine.UIParser.TryParseFileApisClearOutputsOnFailure", [](TestContext& context) {
			const std::filesystem::path directory = MakeParserTempDirectory();
			const std::filesystem::path brokenMarkupPath = directory / "broken.tuiml";
			const std::filesystem::path missingStylePath = directory / "missing.tuss";
			WriteTextFile(brokenMarkupPath, "<UIRoot><Label></UIRoot>");

			const UIParser parser;
			UIElement::Ptr root = UIElement::Create(UIElementType::Root);
			std::string markupError;
			const bool parsedMarkup = parser.TryParseMarkupFile(brokenMarkupPath, root, markupError);

			std::vector<UIStyleRule> rules(1U);
			std::string styleError;
			const bool parsedStyle = parser.TryParseStyleFile(missingStylePath, rules, styleError);

			context.Expect(!parsedMarkup, "TryParseMarkupFile should return false for malformed markup files.");
			context.Expect(root == nullptr, "TryParseMarkupFile should clear the output root after failure.");
			context.Expect(ContainsText(markupError, brokenMarkupPath.generic_string()), "TryParseMarkupFile should include the failing path in its error.");
			context.Expect(!parsedStyle, "TryParseStyleFile should return false for missing stylesheet files.");
			context.Expect(rules.empty(), "TryParseStyleFile should clear stale output rules after failure.");
			context.Expect(ContainsText(styleError, missingStylePath.generic_string()), "TryParseStyleFile should include the missing path in its error.");

			std::filesystem::remove_all(directory);
		});

		registry.Add("Engine.UIParser.ApplyStylesMergesSpecificityHoverAndInlineStyle", [](TestContext& context) {
			const UIParser parser;
			const UIElement::Ptr root = parser.ParseMarkupString(
R"(<UIRoot>
	<Button id="start" class="cta" text="Start" style="opacity: 0.75;" />
</UIRoot>)",
				"StyledTree");
			const std::vector<UIStyleRule> rules = parser.ParseStyleString(
R"(:root { width: 100%; height: 100%; }
Button { opacity: 0.25; }
.cta { color: #00ff00; width: 120px; }
#start { opacity: 0.5; }
#start:hover { scale: 1.25; })");

			parser.ApplyStyles(root, rules);

			const UIElement::Ptr button = root->FindById("start");
			context.Expect(button != nullptr, "The styled button should be findable by id.");
			context.Expect(root->computedStyle.width.has_value() && root->computedStyle.width->unit == UIValue::Unit::Percent, "Root width should be computed from the :root rule.");
			context.Expect(root->computedStyle.height.has_value() && root->computedStyle.height->unit == UIValue::Unit::Percent, "Root height should be computed from the :root rule.");
			context.Expect(button->computedStyle.color.has_value(), "The matching class rule should provide the button color.");
			context.Expect(button->computedStyle.width.has_value() && button->computedStyle.width->unit == UIValue::Unit::Points, "The matching class rule should provide button width.");
			context.Expect(NearlyEqual(button->computedStyle.width->value, 120.0f), "The parsed button width should keep the authored point value.");
			context.Expect(button->computedStyle.opacity.has_value() && NearlyEqual(*button->computedStyle.opacity, 0.75f), "Inline opacity should override lower-priority type and id rules.");
			context.Expect(button->hoverStyle.transform.scale.has_value() && NearlyEqual(*button->hoverStyle.transform.scale, 1.25f), "Hover rules should populate hover style instead of base computed style.");
		});

		registry.Add("Engine.UIParser.SerializeMarkupAndStylesPreservesAuthoredData", [](TestContext& context) {
			const UIParser parser;
			const UIElement::Ptr root = parser.ParseMarkupString(
R"(<UIRoot>
	<Label id="title" class="heading" text="Hello" style="font-size: 18px;" />
</UIRoot>)",
				"SerializableTree");
			const std::vector<UIStyleRule> rules = parser.ParseStyleString(".heading { color: #ffffff; }");

			parser.ApplyStyles(root, rules);

			const std::string markup = parser.SerializeMarkup(root);
			const std::string styles = parser.SerializeStyles(rules);

			context.Expect(ContainsText(markup, "<UIRoot"), "Serialized markup should include the root tag.");
			context.Expect(ContainsText(markup, "id=\"title\""), "Serialized markup should include authored ids.");
			context.Expect(ContainsText(markup, "class=\"heading\""), "Serialized markup should include authored classes.");
			context.Expect(ContainsText(markup, "font-size"), "Serialized markup should include inline style properties.");
			context.Expect(ContainsText(styles, ".heading"), "Serialized styles should include authored selectors.");
			context.Expect(ContainsText(styles, "color"), "Serialized styles should include parsed declarations.");
		});
	}
}
