// UIComponent.h — Component attaching authored UI documents to scene objects.
//
// This component manages the association between objects and UI markup/style
// assets, maintaining the parsed visual tree as transient runtime state. It
// interacts with the UISystem to rebuild and render user interfaces based on
// changes to the source assets or visibility state.

#pragma once

#include "Core/Object/Component.h"
#include "UI/UIElement.h"

#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// UIComponent attaches an authored UI document to an object while keeping the
// parsed visual tree as transient runtime state managed by UISystem.
class UIComponent : public Component<UIComponent>
{
public:
	static constexpr std::string_view kComponentName = "UIComponent";

	// LoadMarkup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void LoadMarkup(const std::filesystem::path& markupPath)
	{
		sourceMarkup = markupPath.generic_string();
		// Changing the authored source invalidates the cached runtime tree so the
		// next UISystem update rebuilds from the newly assigned asset.
		rootElement.reset();
		MarkDirty();
	}

	// SetSourceMarkup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetSourceMarkup(const std::string& markupPath)
	{
		sourceMarkup = markupPath;
		rootElement.reset();
		MarkDirty();
	}

	[[nodiscard]] const std::string& GetSourceMarkup() const
	{
		return sourceMarkup;
	}

	[[nodiscard]] bool HasSourceMarkup() const
	{
		return !sourceMarkup.empty();
	}

	// LoadStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void LoadStyle(const std::filesystem::path& stylePath)
	{
		sourceStyle = stylePath.generic_string();
		rootElement.reset();
		MarkDirty();
	}

	// LoadStyles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void LoadStyles(const std::vector<std::filesystem::path>& stylePaths)
	{
		sourceStyle = JoinSourceStylePaths(stylePaths);
		rootElement.reset();
		MarkDirty();
	}

	// SetSourceStyle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetSourceStyle(const std::string& stylePath)
	{
		sourceStyle = NormalizeSourceStyleList(stylePath);
		rootElement.reset();
		MarkDirty();
	}

	[[nodiscard]] const std::string& GetSourceStyle() const
	{
		return sourceStyle;
	}

	[[nodiscard]] bool HasSourceStyle() const
	{
		return !ParseSourceStyleList(sourceStyle).empty();
	}

	[[nodiscard]] std::vector<std::string> GetSourceStylePaths() const
	{
		return ParseSourceStyleList(sourceStyle);
	}

	// SetVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetVisible(bool nextVisible)
	{
		visible = nextVisible;
		MarkDirty();
	}

	[[nodiscard]] bool IsVisible() const
	{
		return visible;
	}

	// SetRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetRoot(UIElement::Ptr nextRoot)
	{
		rootElement = std::move(nextRoot);
	}

	[[nodiscard]] const UIElement::Ptr& GetRoot() const
	{
		return rootElement;
	}

	// ClearRuntimeTree is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ClearRuntimeTree()
	{
		rootElement.reset();
	}

	// MarkDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void MarkDirty()
	{
		++contentVersion;
	}

	[[nodiscard]] std::uint64_t GetContentVersion() const
	{
		return contentVersion;
	}

	// reflect is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static void reflect(entt::meta_factory<UIComponent>& factory)
	{
		// Only serialize authored source paths and visibility. The parsed UI tree
		// is runtime data that UISystem rebuilds on demand.
		TerraEngine::Reflection::RegisterAccessorProperty<&UIComponent::SetSourceMarkup, &UIComponent::GetSourceMarkup>(factory, "sourceMarkup");
		TerraEngine::Reflection::RegisterAccessorProperty<&UIComponent::SetSourceStyle, &UIComponent::GetSourceStyle>(factory, "sourceStyle");
		TerraEngine::Reflection::RegisterAccessorProperty<&UIComponent::SetVisible, &UIComponent::IsVisible>(factory, "visible");
	}

private:
	// TrimSourceStyleToken is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static std::string TrimSourceStyleToken(std::string_view token)
	{
		std::size_t begin = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (begin < token.size() && std::isspace(static_cast<unsigned char>(token[begin])) != 0)
		{
			++begin;
		}

		std::size_t end = token.size();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])) != 0)
		{
			--end;
		}

		return std::string(token.substr(begin, end - begin));
	}

	// ParseSourceStyleList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static std::vector<std::string> ParseSourceStyleList(std::string_view serializedPaths)
	{
		std::vector<std::string> parsedPaths;
		std::size_t cursor = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (cursor <= serializedPaths.size())
		{
			const std::size_t delimiter = serializedPaths.find(';', cursor);
			const std::string cleanedToken = TrimSourceStyleToken(serializedPaths.substr(
				cursor,
				delimiter == std::string_view::npos ? serializedPaths.size() - cursor : delimiter - cursor));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!cleanedToken.empty())
			{
				parsedPaths.push_back(cleanedToken);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (delimiter == std::string_view::npos)
			{
				break;
			}

			cursor = delimiter + 1;
		}

		return parsedPaths;
	}

	// NormalizeSourceStyleList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static std::string NormalizeSourceStyleList(std::string_view serializedPaths)
	{
		std::string normalized;
		const std::vector<std::string> parsedPaths = ParseSourceStyleList(serializedPaths);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t index = 0; index < parsedPaths.size(); ++index)
		{
			if (index > 0)
			{
				normalized += ';';
			}
			normalized += parsedPaths[index];
		}

		return normalized;
	}

	// JoinSourceStylePaths is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static std::string JoinSourceStylePaths(const std::vector<std::filesystem::path>& stylePaths)
	{
		std::string serializedPaths;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t index = 0; index < stylePaths.size(); ++index)
		{
			if (index > 0)
			{
				serializedPaths += ';';
			}
			serializedPaths += stylePaths[index].generic_string();
		}

		return NormalizeSourceStyleList(serializedPaths);
	}

	std::string sourceMarkup;
	std::string sourceStyle;
	bool visible = true;
	UIElement::Ptr rootElement;
	std::uint64_t contentVersion = 1;
};
