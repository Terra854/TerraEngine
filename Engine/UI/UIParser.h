// UIParser.h — Parser for XML-based markup and CSS-like stylesheets.
//
// This file defines the UIParser class, which is responsible for converting
// authored XML files and style definitions into a live UIElement visual tree.
// It handles attribute parsing, class-based styling, and asset reference
// resolution for UI components.

#pragma once

#include "UIElement.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// UIParser owns `.tuiml` and `.tuss` parsing so UISystem and editor tooling can
// share one implementation for loading, styling, and exporting UI assets.
class UIParser
{
public:
	UIElement::Ptr ParseMarkupFile(const std::filesystem::path& markupPath) const;
	UIElement::Ptr ParseMarkupString(std::string_view markupText, std::string_view debugName = "Preview") const;
	bool TryParseMarkupFile(const std::filesystem::path& markupPath, UIElement::Ptr& outRoot, std::string& outError) const;

	std::vector<UIStyleRule> ParseStyleFile(const std::filesystem::path& stylePath) const;
	std::vector<UIStyleRule> ParseStyleString(std::string_view styleText) const;
	bool TryParseStyleFile(const std::filesystem::path& stylePath, std::vector<UIStyleRule>& outRules, std::string& outError) const;
	bool TryParseSelector(std::string_view selectorText, UIStyleRule& outRule) const;

	void ApplyStyles(const UIElement::Ptr& root, const std::vector<UIStyleRule>& rules) const;

	std::string SerializeMarkup(const UIElement::Ptr& root) const;
	std::string SerializeStyles(const UIElement::Ptr& root) const;
	std::string SerializeStyles(const std::vector<UIStyleRule>& rules) const;
};
