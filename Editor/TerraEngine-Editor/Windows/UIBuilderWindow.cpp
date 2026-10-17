// UIBuilderWindow.cpp — UI Builder Window Implementation
//
// Implements the visual UI editing interface, supporting drag-and-drop element placement and property editing.
// It generates and parses UI configuration files that are used by the engine's runtime UI system.

#include "UIBuilderWindow.h"

#include "AssetDatabase.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Components/UIComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/UI/UISystem.h"

#include <imgui.h>
#include <ImGuiFileDialog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace
{
constexpr char kCreateStyleSheetDialogKey[] = "CreateUIStyleSheetDlg";

bool EqualsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs)
{
	if (lhs.size() != rhs.size())
	{
		return false;
	}

	for (std::size_t index = 0; index < lhs.size(); ++index)
	{
		// UI selector type names are authored as ASCII identifiers, so avoid
		// platform-specific functions such as _stricmp while keeping the
		// comparison predictable across Windows and Linux.
		const unsigned char left = static_cast<unsigned char>(lhs[index]);
		const unsigned char right = static_cast<unsigned char>(rhs[index]);
		if (std::tolower(left) != std::tolower(right))
		{
			return false;
		}
	}

	return true;
}


ImU32 ToImColor(const glm::vec4& color)
{
	return IM_COL32(
		static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
		static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
		static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
		static_cast<int>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f));
}


void CopyStringToBuffer(const std::string& source, char* destination, std::size_t destinationSize)
{
	if (destination == nullptr || destinationSize == 0)
	{
		return;
	}

	std::snprintf(destination, destinationSize, "%s", source.c_str());
}


bool IsPathInsideRoot(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	if (root.empty() || candidate.empty())
	{
		return false;
	}

	std::error_code errorCode;
	const std::filesystem::path relativePath = std::filesystem::relative(candidate, root, errorCode);

	if (errorCode)
	{
		return false;
	}

	const std::string generic = relativePath.generic_string();
	return !generic.empty() && generic.rfind("..", 0) != 0;
}


std::filesystem::path NormalizeToAssetRelativePath(const std::filesystem::path& assetRoot, const std::filesystem::path& authoredPath)
{
	if (authoredPath.empty())
	{
		return {};
	}


	if (!authoredPath.is_absolute())
	{
		return authoredPath.lexically_normal();
	}


	if (IsPathInsideRoot(assetRoot, authoredPath))
	{
		std::error_code errorCode;
		const std::filesystem::path relativePath = std::filesystem::relative(authoredPath, assetRoot, errorCode);

		if (!errorCode)
		{
			return relativePath.lexically_normal();
		}
	}

	return authoredPath.lexically_normal();
}


std::string BuildDisplayName(const std::filesystem::path& assetPath, std::string_view fallbackName)
{
	if (assetPath.empty())
	{
		return std::string(fallbackName);
	}

	const std::filesystem::path fileName = assetPath.filename();
	return fileName.empty() ? assetPath.generic_string() : fileName.string();
}


std::filesystem::path InferStylePathFromMarkup(const std::filesystem::path& markupPath)
{
	std::filesystem::path stylePath = markupPath;
	stylePath.replace_extension(".tuss");
	return stylePath;
}


std::optional<std::filesystem::file_time_type> TryGetLastWriteTime(const std::filesystem::path& filePath)
{
	if (filePath.empty() || !std::filesystem::exists(filePath))
	{
		return std::nullopt;
	}

	std::error_code errorCode;
	const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(filePath, errorCode);

	if (errorCode)
	{
		return std::nullopt;
	}

	return writeTime;
}


UIElement::Ptr CreateEmptyRoot()
{
	UIElement::Ptr root = UIElement::Create(UIElementType::Root);
	root->typeName = "UIRoot";
	return root;
}


void DrawUIBatchPreview(const UIRenderBatch& batch, ImDrawList* drawList, const ImVec2& imageMin, const ImVec2& imageSize)
{
	if ((batch.viewportSize.x <= 0.0f || batch.viewportSize.y <= 0.0f) || drawList == nullptr)
	{
		return;
	}

	const ImVec2 scale(imageSize.x / batch.viewportSize.x, imageSize.y / batch.viewportSize.y);
	drawList->PushClipRect(imageMin, ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y), true);


	for (const UIStaticQuad& quad : batch.staticQuads)
	{
		const ImVec2 min(imageMin.x + (quad.position.x * scale.x), imageMin.y + (quad.position.y * scale.y));
		const ImVec2 max(min.x + (quad.size.x * scale.x), min.y + (quad.size.y * scale.y));
		drawList->AddRectFilled(min, max, ToImColor(quad.color), quad.cornerRadius * std::min(scale.x, scale.y));
	}


	for (const UITextRun& textRun : batch.textRuns)
	{
		const ImVec2 position(imageMin.x + (textRun.position.x * scale.x), imageMin.y + (textRun.position.y * scale.y));
		drawList->AddText(nullptr, textRun.fontSize * scale.y, position, ToImColor(textRun.color), textRun.text.c_str());
	}

	drawList->PopClipRect();
}


glm::vec2 GetPreviewElementMin(const UIElement& element)
{
	const glm::vec2 translate = element.displayStyle.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	return element.calculatedPosition + translate;
}


glm::vec2 GetPreviewElementSize(const UIElement& element)
{
	const float scale = element.displayStyle.transform.scale.value_or(1.0f);
	return element.calculatedSize * scale;
}


bool BuildElementPath(const UIElement::Ptr& root, const UIElement* target, std::vector<std::size_t>& outPath)
{
	if (!root || target == nullptr)
	{
		return false;
	}


	if (root.get() == target)
	{
		outPath.clear();
		return true;
	}


	for (std::size_t index = 0; index < root->children.size(); ++index)
	{
		std::vector<std::size_t> childPath;

		if (BuildElementPath(root->children[index], target, childPath))
		{
			outPath.clear();
			outPath.push_back(index);
			outPath.insert(outPath.end(), childPath.begin(), childPath.end());
			return true;
		}
	}

	return false;
}


UIElement::Ptr ResolveElementAtPath(const UIElement::Ptr& root, const std::vector<std::size_t>& path)
{
	UIElement::Ptr current = root;

	for (std::size_t index : path)
	{
		if (!current || index >= current->children.size())
		{
			return nullptr;
		}

		current = current->children[index];
	}

	return current;
}


bool HitTestPreviewElement(const UIElement::Ptr& element, const glm::vec2& localPoint, std::vector<std::size_t>& currentPath, std::vector<std::size_t>& outPath)
{
	if (!element)
	{
		return false;
	}

	const glm::vec2 min = GetPreviewElementMin(*element);
	const glm::vec2 max = min + GetPreviewElementSize(*element);
	const bool inside = localPoint.x >= min.x
		&& localPoint.y >= min.y
		&& localPoint.x <= max.x
		&& localPoint.y <= max.y;

	if (!inside)
	{
		return false;
	}


	for (std::size_t reverseIndex = element->children.size(); reverseIndex > 0; --reverseIndex)
	{
		const std::size_t childIndex = reverseIndex - 1;
		currentPath.push_back(childIndex);

		if (HitTestPreviewElement(element->children[childIndex], localPoint, currentPath, outPath))
		{
			return true;
		}
		currentPath.pop_back();
	}

	outPath = currentPath;
	return true;
}


bool SelectorTargetsElement(const UIStyleRule& rule, const UIElement& element)
{
	// The inspector must only offer rules that can actually affect the selected
	// element; otherwise the user ends up editing an unrelated selector.

	if (rule.validity != UISelectorValidityState::Valid)
	{
		return false;
	}

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (rule.selectorKind)
	{
	case UISelectorKind::Root:
		return element.type == UIElementType::Root;
	case UISelectorKind::Id:
		return element.id == rule.selectorValue;
	case UISelectorKind::Class:
		return element.HasClass(rule.selectorValue);
	case UISelectorKind::Type:
	default:
		return EqualsIgnoreAsciiCase(element.typeName, rule.selectorValue);
	}
}


UIElement::Ptr FindElementByPointer(const UIElement::Ptr& root, const UIElement* target)
{
	if (!root || target == nullptr)
	{
		return nullptr;
	}


	if (root.get() == target)
	{
		return root;
	}


	for (const UIElement::Ptr& child : root->children)
	{
		if (UIElement::Ptr found = FindElementByPointer(child, target))
		{
			return found;
		}
	}

	return nullptr;
}

constexpr std::array<const char*, 5> kLibraryCategoryLabels = {
	"All",
	"Containers",
	"Text",
	"Controls",
	"Media",
};

constexpr std::array<const char*, 3> kFlexDirectionLabels = {
	"Unset",
	"Row",
	"Column",
};

constexpr std::array<const char*, 6> kJustifyContentLabels = {
	"Unset",
	"Flex Start",
	"Center",
	"Flex End",
	"Space Between",
	"Space Around",
};

constexpr std::array<const char*, 5> kAlignItemsLabels = {
	"Unset",
	"Stretch",
	"Flex Start",
	"Center",
	"Flex End",
};


int FlexDirectionToIndex(const std::optional<UIFlexDirection>& value)
{
	if (!value.has_value())
	{
		return 0;
	}

	return *value == UIFlexDirection::Row ? 1 : 2;
}


int JustifyContentToIndex(const std::optional<UIJustifyContent>& value)
{
	if (!value.has_value())
	{
		return 0;
	}

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (*value)
	{
	case UIJustifyContent::FlexStart: return 1;
	case UIJustifyContent::Center: return 2;
	case UIJustifyContent::FlexEnd: return 3;
	case UIJustifyContent::SpaceBetween: return 4;
	case UIJustifyContent::SpaceAround: return 5;
	default: return 0;
	}
}


int AlignItemsToIndex(const std::optional<UIAlignItems>& value)
{
	if (!value.has_value())
	{
		return 0;
	}

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (*value)
	{
	case UIAlignItems::Stretch: return 1;
	case UIAlignItems::FlexStart: return 2;
	case UIAlignItems::Center: return 3;
	case UIAlignItems::FlexEnd: return 4;
	default: return 0;
	}
}
}

UIBuilderWindow::UIBuilderWindow(MainEditorSubsystem* owner)

	: EditorWindow("UI Builder", true, "Tools")
	, m_Owner(owner)
{
	CreateNewDocumentInternal();
}


void UIBuilderWindow::OpenMarkupDocument(const std::filesystem::path& markupPath)
{
	SetVisible(true);

	if (HasUnsavedChanges())
	{
		BeginPrompt(UIBuilderPendingPrompt::OpenMarkupAsset, markupPath);
		return;
	}

	OpenMarkupDocumentInternal(markupPath);
}


void UIBuilderWindow::ResetForProjectOpen()
{
	ClearPrompt();
	CreateNewDocumentInternal();
	m_StatusMessage = "Project opened. Double-click a .tuiml asset or load a UIComponent to begin editing.";
}


bool UIBuilderWindow::OnCloseRequested()
{
	if (!HasUnsavedChanges())
	{
		return true;
	}

	BeginPrompt(UIBuilderPendingPrompt::CloseWindow);
	return false;
}


void UIBuilderWindow::RequestNewDocument()
{
	if (HasUnsavedChanges())
	{
		BeginPrompt(UIBuilderPendingPrompt::NewDocument);
		return;
	}

	CreateNewDocumentInternal();
}


void UIBuilderWindow::RequestLoadSelectedComponent()
{
	SetVisible(true);
	const std::optional<entt::entity> selectedEntity = m_Owner == nullptr
		? std::optional<entt::entity>{}
		: m_Owner->GetSelectedEntity();

	if (!selectedEntity.has_value())
	{
		m_StatusMessage = "Select an object with a UIComponent first.";
		return;
	}


	if (HasUnsavedChanges())
	{
		BeginPrompt(UIBuilderPendingPrompt::LoadSelectedComponent, {}, *selectedEntity);
		return;
	}

	LoadSelectedComponentInternal(*selectedEntity);
}


std::filesystem::path UIBuilderWindow::GetMarkupAssetPath() const
{
	return m_DocumentSession.internalPath;
}


std::filesystem::path UIBuilderWindow::GetStyleAssetPath() const
{
	if (const StyleSheetSession* selectedSession = GetSelectedStyleSheetSession())
	{
		return selectedSession->internalPath;
	}

	return {};
}


std::filesystem::path UIBuilderWindow::ResolveAuthoredPath(const std::filesystem::path& authoredPath) const
{
	if (authoredPath.empty())
	{
		return {};
	}


	if (authoredPath.is_absolute())
	{
		return authoredPath;
	}

	return ResourceManager::Instance().GetAssetRoot() / authoredPath;
}


std::string UIBuilderWindow::GetMarkupDisplayName() const
{
	return m_DocumentSession.displayName;
}


std::string UIBuilderWindow::GetStyleSheetDisplayName() const
{
	if (const StyleSheetSession* selectedSession = GetSelectedStyleSheetSession())
	{
		return selectedSession->displayName;
	}

	return m_DocumentSession.styleSheets.empty() ? "None" : m_DocumentSession.styleSheets.front().displayName;
}


void UIBuilderWindow::RefreshSessionDisplayNames()
{
	m_DocumentSession.displayName = BuildDisplayName(m_DocumentSession.internalPath, "Unsaved UI");

	for (StyleSheetSession& styleSession : m_DocumentSession.styleSheets)
	{
		if (!styleSession.attached)
		{
			styleSession.displayName = styleSession.rules.empty() ? "None" : "Unsaved StyleSheet";
			continue;
		}


		if (!styleSession.internalPath.empty())
		{
			styleSession.displayName = BuildDisplayName(styleSession.internalPath, "StyleSheet");
		}
		else
		{
			styleSession.displayName = "Unsaved StyleSheet";
		}
	}
}


void UIBuilderWindow::CaptureSessionRevisions()
{
	m_DocumentSession.revisionStamp = TryGetLastWriteTime(ResolveAuthoredPath(m_DocumentSession.internalPath));

	for (StyleSheetSession& styleSession : m_DocumentSession.styleSheets)
	{
		styleSession.revisionStamp = std::nullopt;

		if (styleSession.attached || styleSession.removeOnNextSave)
		{
			styleSession.revisionStamp = TryGetLastWriteTime(ResolveAuthoredPath(styleSession.internalPath));
		}
	}
}


void UIBuilderWindow::MarkDocumentDirty(std::string_view reason)
{
	m_DocumentSession.dirty = true;

	if (!reason.empty())
	{
		m_StatusMessage = std::string(reason);
	}
	QueuePreviewRefresh();
}


void UIBuilderWindow::MarkStyleSheetDirty(std::string_view reason)
{
	StyleSheetSession* styleSession = GetSelectedStyleSheetSession();

	if (styleSession == nullptr)
	{
		const int styleSheetIndex = EnsureSelectedStyleSheetSession();
		styleSession = styleSheetIndex >= 0 ? &m_DocumentSession.styleSheets[static_cast<std::size_t>(styleSheetIndex)] : nullptr;
	}


	if (styleSession == nullptr)
	{
		return;
	}

	styleSession->attached = true;
	styleSession->dirty = true;
	styleSession->removeOnNextSave = false;
	RefreshSessionDisplayNames();

	if (!reason.empty())
	{
		m_StatusMessage = std::string(reason);
	}
	QueuePreviewRefresh();
}


bool UIBuilderWindow::HasUnsavedChanges() const
{
	if (m_DocumentSession.dirty)
	{
		return true;
	}

	return std::any_of(
		m_DocumentSession.styleSheets.begin(),
		m_DocumentSession.styleSheets.end(),
		[](const StyleSheetSession& styleSession) { return styleSession.dirty; });
}


void UIBuilderWindow::QueuePreviewRefresh()
{
	m_PreviewDirty = true;
}


void UIBuilderWindow::ApplyWorkingStyles()
{
	if (!m_DocumentSession.root)
	{
		return;
	}

	m_Parser.ApplyStyles(m_DocumentSession.root, BuildCombinedStyleRules());
	QueuePreviewRefresh();
}


bool UIBuilderWindow::CreateStyleRule(std::string_view selectorText, bool selectNewRule)
{
	const int styleSheetIndex = EnsureSelectedStyleSheetSession();

	if (styleSheetIndex < 0)
	{
		m_StatusMessage = "Create or attach a stylesheet before adding selectors.";
		return false;
	}

	UIStyleRule parsedRule;

	if (!m_Parser.TryParseSelector(selectorText, parsedRule))
	{
		m_StatusMessage = "Unsupported selector. Use :root, element names, #ids, .classes, and optional :hover.";
		return false;
	}

	StyleSheetSession& styleSession = m_DocumentSession.styleSheets[static_cast<std::size_t>(styleSheetIndex)];
	parsedRule.order = styleSession.rules.size();
	styleSession.rules.push_back(std::move(parsedRule));

	if (selectNewRule)
	{
		SelectStyleRule(styleSheetIndex, static_cast<int>(styleSession.rules.size()) - 1);
	}
	ApplyWorkingStyles();
	MarkStyleSheetDirty("Added selector: " + styleSession.rules.back().selectorText);
	return true;
}


void UIBuilderWindow::SelectStyleRule(int styleSheetIndex, int styleRuleIndex, bool focusStyleSheet)
{
	if (styleSheetIndex < 0 || styleSheetIndex >= static_cast<int>(m_DocumentSession.styleSheets.size()))
	{
		m_WorkspaceState.selectedStyleSheetIndex = -1;
		m_WorkspaceState.selectedSelectorIndex = -1;
		return;
	}

	const StyleSheetSession& styleSession = m_DocumentSession.styleSheets[static_cast<std::size_t>(styleSheetIndex)];

	if (styleRuleIndex < 0 || styleRuleIndex >= static_cast<int>(styleSession.rules.size()))
	{
		m_WorkspaceState.selectedStyleSheetIndex = styleSheetIndex;
		m_WorkspaceState.selectedSelectorIndex = -1;
		return;
	}

	m_WorkspaceState.selectedStyleSheetIndex = styleSheetIndex;
	m_WorkspaceState.selectedSelectorIndex = styleRuleIndex;

	if (focusStyleSheet)
	{
		m_WorkspaceState.activePanel = UIBuilderPanelFocus::StyleSheets;
	}
}


void UIBuilderWindow::SetSelectedElement(const UIElement::Ptr& element, UIBuilderPanelFocus focus)
{
	// Unity treats element and selector inspection as separate modes, so moving
	// back to an element selection should clear any stale selector selection
	// without forgetting which attached stylesheet is currently active.
	m_WorkspaceState.selectedElement = element;
	m_WorkspaceState.selectedSelectorIndex = -1;
	m_WorkspaceState.activePanel = focus;
}


std::vector<MatchingStyleRuleReference> UIBuilderWindow::CollectMatchingRuleIndices(const UIElement& element) const
{
	std::vector<MatchingStyleRuleReference> matchingIndices;
	// Keep the rule list in stylesheet order so later tie-breaking can respect
	// the same precedence the runtime uses when multiple selectors match.

	for (std::size_t styleSheetIndex = 0; styleSheetIndex < m_DocumentSession.styleSheets.size(); ++styleSheetIndex)
	{
		const StyleSheetSession& styleSession = m_DocumentSession.styleSheets[styleSheetIndex];

		for (std::size_t index = 0; index < styleSession.rules.size(); ++index)
		{
			if (SelectorTargetsElement(styleSession.rules[index], element))
			{
				matchingIndices.push_back(MatchingStyleRuleReference{
					static_cast<int>(styleSheetIndex),
					static_cast<int>(index),
				});
			}
		}
	}

	return matchingIndices;
}


int UIBuilderWindow::EnsureSelectedStyleSheetSession()
{
	if (m_WorkspaceState.selectedStyleSheetIndex >= 0
		&& m_WorkspaceState.selectedStyleSheetIndex < static_cast<int>(m_DocumentSession.styleSheets.size()))
	{
		return m_WorkspaceState.selectedStyleSheetIndex;
	}


	if (!m_DocumentSession.styleSheets.empty())
	{
		m_WorkspaceState.selectedStyleSheetIndex = 0;
		return 0;
	}

	// Do not silently invent a stylesheet here. The builder should force the
	// user through Create or Attach so the authored .tuss path is explicit.
	return -1;
}


StyleSheetSession* UIBuilderWindow::GetSelectedStyleSheetSession()
{
	if (m_WorkspaceState.selectedStyleSheetIndex < 0
		|| m_WorkspaceState.selectedStyleSheetIndex >= static_cast<int>(m_DocumentSession.styleSheets.size()))
	{
		return nullptr;
	}

	return &m_DocumentSession.styleSheets[static_cast<std::size_t>(m_WorkspaceState.selectedStyleSheetIndex)];
}


const StyleSheetSession* UIBuilderWindow::GetSelectedStyleSheetSession() const
{
	if (m_WorkspaceState.selectedStyleSheetIndex < 0
		|| m_WorkspaceState.selectedStyleSheetIndex >= static_cast<int>(m_DocumentSession.styleSheets.size()))
	{
		return nullptr;
	}

	return &m_DocumentSession.styleSheets[static_cast<std::size_t>(m_WorkspaceState.selectedStyleSheetIndex)];
}


std::vector<UIStyleRule> UIBuilderWindow::BuildCombinedStyleRules() const
{
	std::vector<UIStyleRule> combinedRules;

	for (const StyleSheetSession& styleSession : m_DocumentSession.styleSheets)
	{
		const std::size_t orderOffset = combinedRules.size();

		for (const UIStyleRule& rule : styleSession.rules)
		{
			UIStyleRule copiedRule = rule;
			copiedRule.order += orderOffset;
			combinedRules.push_back(std::move(copiedRule));
		}
	}

	return combinedRules;
}


std::filesystem::path UIBuilderWindow::SuggestNewStyleSheetPath() const
{
	const std::filesystem::path markupPath = ResolveAuthoredPath(m_DocumentSession.internalPath);

	if (markupPath.empty())
	{
		return {};
	}

	std::filesystem::path candidate = InferStylePathFromMarkup(markupPath);
	auto pathAlreadyUsed = [&](const std::filesystem::path& authoredPath) {
		return std::any_of(
			m_DocumentSession.styleSheets.begin(),
			m_DocumentSession.styleSheets.end(),
			[&](const StyleSheetSession& styleSession) {
				return !styleSession.internalPath.empty()
					&& ResolveAuthoredPath(styleSession.internalPath).lexically_normal() == authoredPath.lexically_normal();
			});
	};


	if (!pathAlreadyUsed(candidate))
	{
		return candidate;
	}

	const std::filesystem::path parentPath = candidate.parent_path();
	const std::string baseName = candidate.stem().string();

	for (int suffixIndex = 2; suffixIndex < 128; ++suffixIndex)
	{
		const std::filesystem::path numberedCandidate = parentPath / (baseName + "-" + std::to_string(suffixIndex) + ".tuss");

		if (!pathAlreadyUsed(numberedCandidate))
		{
			return numberedCandidate;
		}
	}

	return candidate;
}


bool UIBuilderWindow::AttachStyleSheetAsset(const std::filesystem::path& stylePath, bool selectAttachedSheet)
{
	if (stylePath.empty())
	{
		m_StatusMessage = "Choose a valid .tuss asset to attach.";
		return false;
	}

	const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();
	const std::filesystem::path resolvedStylePath = ResolveAuthoredPath(stylePath);
	const std::filesystem::path normalizedStylePath = NormalizeToAssetRelativePath(assetRoot, resolvedStylePath);

	const auto existingIterator = std::find_if(
		m_DocumentSession.styleSheets.begin(),
		m_DocumentSession.styleSheets.end(),
		[&](const StyleSheetSession& styleSession) {
			return !styleSession.internalPath.empty()
				&& ResolveAuthoredPath(styleSession.internalPath).lexically_normal() == resolvedStylePath.lexically_normal();
		});

	if (existingIterator != m_DocumentSession.styleSheets.end())
	{
		if (selectAttachedSheet)
		{
			m_WorkspaceState.selectedStyleSheetIndex = static_cast<int>(std::distance(m_DocumentSession.styleSheets.begin(), existingIterator));
		}
		m_StatusMessage = "That stylesheet is already attached to the current UI document.";
		return false;
	}

	StyleSheetSession newSession;
	newSession.attached = true;
	newSession.internalPath = normalizedStylePath;
	newSession.displayName = BuildDisplayName(normalizedStylePath, "StyleSheet");
	newSession.removeOnNextSave = false;

	if (std::filesystem::exists(resolvedStylePath))
	{
		if (!m_Parser.TryParseStyleFile(resolvedStylePath, newSession.rules, newSession.loadError))
		{
			m_StatusMessage = "Attached the stylesheet, but it needs attention: " + newSession.loadError;
		}
	}
	else
	{
		newSession.loadError = "Referenced stylesheet not found: " + resolvedStylePath.generic_string();
	}

	m_DocumentSession.styleSheets.push_back(std::move(newSession));
	const int attachedIndex = static_cast<int>(m_DocumentSession.styleSheets.size()) - 1;

	if (selectAttachedSheet)
	{
		m_WorkspaceState.selectedStyleSheetIndex = attachedIndex;
		m_WorkspaceState.selectedSelectorIndex = -1;
	}

	RefreshSessionDisplayNames();
	CaptureSessionRevisions();
	ApplyWorkingStyles();
	MarkDocumentDirty("Attached stylesheet: " + resolvedStylePath.filename().string());

	if (m_StatusMessage.empty())
	{
		m_StatusMessage = "Attached stylesheet: " + resolvedStylePath.filename().string();
	}
	return true;
}


UIStyleRule* UIBuilderWindow::GetSelectedStyleRule()
{
	StyleSheetSession* styleSession = GetSelectedStyleSheetSession();

	if (styleSession == nullptr
		|| m_WorkspaceState.selectedSelectorIndex < 0
		|| m_WorkspaceState.selectedSelectorIndex >= static_cast<int>(styleSession->rules.size()))
	{
		return nullptr;
	}

	return &styleSession->rules[static_cast<std::size_t>(m_WorkspaceState.selectedSelectorIndex)];
}


const UIStyleRule* UIBuilderWindow::GetSelectedStyleRule() const
{
	const StyleSheetSession* styleSession = GetSelectedStyleSheetSession();

	if (styleSession == nullptr
		|| m_WorkspaceState.selectedSelectorIndex < 0
		|| m_WorkspaceState.selectedSelectorIndex >= static_cast<int>(styleSession->rules.size()))
	{
		return nullptr;
	}

	return &styleSession->rules[static_cast<std::size_t>(m_WorkspaceState.selectedSelectorIndex)];
}


UIElement::Ptr UIBuilderWindow::GetSelectedWorkingElement() const
{
	if (UIElement::Ptr selected = m_WorkspaceState.selectedElement.lock())
	{
		return selected;
	}

	return m_DocumentSession.root;
}


UIElement::Ptr UIBuilderWindow::GetSelectedInsertionParent() const
{
	if (UIElement::Ptr selected = GetSelectedWorkingElement())
	{
		return selected;
	}

	return m_DocumentSession.root;
}


bool UIBuilderWindow::OpenMarkupDocumentInternal(const std::filesystem::path& markupPath)
{
	m_StatusMessage.clear();
	const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();
	const std::filesystem::path resolvedMarkupPath = ResolveAuthoredPath(markupPath);

	BuilderDocumentSession nextSession;
	nextSession.entryContext = UIBuilderEntryContext::AssetBrowser;
	nextSession.internalPath = NormalizeToAssetRelativePath(assetRoot, resolvedMarkupPath);

	std::string markupError;

	if (!m_Parser.TryParseMarkupFile(resolvedMarkupPath, nextSession.root, markupError))
	{
		nextSession.root = CreateEmptyRoot();
		m_StatusMessage = markupError + " Loaded an empty UIRoot so the document can be repaired.";
	}

	std::vector<std::filesystem::path> authoredStylePaths;

	if (nextSession.root && !nextSession.root->styleSheetSources.empty())
	{
		for (const std::string& styleSource : nextSession.root->styleSheetSources)
		{
			authoredStylePaths.push_back(NormalizeToAssetRelativePath(assetRoot, ResolveAuthoredPath(styleSource)));
		}
	}
	else
	{
		const std::filesystem::path inferredStylePath = InferStylePathFromMarkup(resolvedMarkupPath);

		if (std::filesystem::exists(inferredStylePath))
		{
			authoredStylePaths.push_back(NormalizeToAssetRelativePath(assetRoot, inferredStylePath));
		}
	}


	for (const std::filesystem::path& authoredStylePath : authoredStylePaths)
	{
		StyleSheetSession styleSession;
		styleSession.attached = true;
		styleSession.internalPath = authoredStylePath;
		const std::filesystem::path resolvedAuthoredStylePath = ResolveAuthoredPath(authoredStylePath);

		if (!m_Parser.TryParseStyleFile(resolvedAuthoredStylePath, styleSession.rules, styleSession.loadError))
		{
			m_StatusMessage = "Loaded the document, but one stylesheet needs attention: " + styleSession.loadError;
		}

		nextSession.styleSheets.push_back(std::move(styleSession));
	}

	{
		std::vector<UIStyleRule> combinedRules;

		for (const StyleSheetSession& styleSession : nextSession.styleSheets)
		{
			const std::size_t orderOffset = combinedRules.size();

			for (const UIStyleRule& rule : styleSession.rules)
			{
				UIStyleRule copiedRule = rule;
				copiedRule.order += orderOffset;
				combinedRules.push_back(std::move(copiedRule));
			}
		}
		m_Parser.ApplyStyles(nextSession.root, combinedRules);
	}
	nextSession.dirty = false;

	m_DocumentSession = std::move(nextSession);
	RefreshSessionDisplayNames();
	CaptureSessionRevisions();
	SetSelectedElement(m_DocumentSession.root, UIBuilderPanelFocus::Hierarchy);
	m_WorkspaceState.selectedStyleSheetIndex = m_DocumentSession.styleSheets.empty() ? -1 : 0;
	m_WorkspaceState.selectedSelectorIndex = -1;
	m_WorkspaceState.previewHover = false;
	m_NewSelectorBuffer[0] = '\0';
	QueuePreviewRefresh();


	if (m_StatusMessage.empty())
	{
		m_StatusMessage = "Loaded UI asset: " + resolvedMarkupPath.filename().string();
	}

	SetVisible(true);
	return true;
}


bool UIBuilderWindow::LoadSelectedComponentInternal(entt::entity targetEntity)
{
	m_StatusMessage.clear();

	if (m_Owner == nullptr)
	{
		m_StatusMessage = "Select an object with a UIComponent first.";
		return false;
	}


	if (targetEntity == entt::null)
	{
		const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity();

		if (!selectedEntity.has_value())
		{
			m_StatusMessage = "Select an object with a UIComponent first.";
			return false;
		}

		targetEntity = *selectedEntity;
	}

	const entt::entity entity = targetEntity;
	auto& objectManager = ObjectManager::instance();

	std::optional<Object> object = objectManager.getObject(entity);
	if (!object)
	{
		m_StatusMessage = "The selected object no longer exists.";
		return false;
	}

	UIComponent* component = objectManager.GetComponent<UIComponent>(*object);

	if (component == nullptr)
	{
		m_StatusMessage = "The selected object does not have a UIComponent.";
		return false;
	}

	const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();
	BuilderDocumentSession nextSession;
	nextSession.entryContext = UIBuilderEntryContext::SceneComponent;
	nextSession.sceneEntity = entity;


	if (component->HasSourceMarkup())
	{
		const std::filesystem::path authoredMarkupPath(component->GetSourceMarkup());
		const std::filesystem::path resolvedMarkupPath = ResolveAuthoredPath(authoredMarkupPath);
		nextSession.internalPath = NormalizeToAssetRelativePath(assetRoot, resolvedMarkupPath);

		std::string markupError;

		if (!m_Parser.TryParseMarkupFile(resolvedMarkupPath, nextSession.root, markupError))
		{
			if (component->GetRoot())
			{
				nextSession.root = component->GetRoot()->Clone();
				m_StatusMessage = markupError + " The runtime tree was used as recovery content.";
			}
			else
			{
				nextSession.root = CreateEmptyRoot();
				m_StatusMessage = markupError + " An empty UIRoot was loaded for recovery.";
			}
		}


		if (nextSession.root && !nextSession.root->styleSheetSources.empty())
		{
			for (const std::string& styleSource : nextSession.root->styleSheetSources)
			{
				nextSession.styleSheets.push_back(StyleSheetSession{
					NormalizeToAssetRelativePath(assetRoot, ResolveAuthoredPath(styleSource))
				});
			}
		}
		else
		{
			const std::filesystem::path inferredStylePath = InferStylePathFromMarkup(resolvedMarkupPath);

			if (std::filesystem::exists(inferredStylePath))
			{
				nextSession.styleSheets.push_back(StyleSheetSession{
					NormalizeToAssetRelativePath(assetRoot, inferredStylePath)
				});
			}
		}


		for (StyleSheetSession& styleSession : nextSession.styleSheets)
		{
			const std::filesystem::path resolvedStylePath = ResolveAuthoredPath(styleSession.internalPath);
			styleSession.attached = true;

			if (std::filesystem::exists(resolvedStylePath))
			{
				if (!m_Parser.TryParseStyleFile(resolvedStylePath, styleSession.rules, styleSession.loadError))
				{
					m_StatusMessage = "Loaded the UIComponent, but one stylesheet needs attention: " + styleSession.loadError;
				}
			}
			else
			{
				styleSession.loadError = "Referenced stylesheet not found: " + resolvedStylePath.generic_string();
				m_StatusMessage = styleSession.loadError;
			}
		}
	}

	else if (component->GetRoot())
	{
		nextSession.root = component->GetRoot()->Clone();
	}
	else
	{
		nextSession.root = CreateEmptyRoot();
		m_StatusMessage = "The selected UIComponent has no authored document yet. An empty UIRoot was loaded.";
	}

	{
		std::vector<UIStyleRule> combinedRules;

		for (const StyleSheetSession& styleSession : nextSession.styleSheets)
		{
			const std::size_t orderOffset = combinedRules.size();

			for (const UIStyleRule& rule : styleSession.rules)
			{
				UIStyleRule copiedRule = rule;
				copiedRule.order += orderOffset;
				combinedRules.push_back(std::move(copiedRule));
			}
		}
		m_Parser.ApplyStyles(nextSession.root, combinedRules);
	}
	nextSession.dirty = false;

	m_DocumentSession = std::move(nextSession);
	RefreshSessionDisplayNames();
	CaptureSessionRevisions();
	SetSelectedElement(m_DocumentSession.root, UIBuilderPanelFocus::Hierarchy);
	m_WorkspaceState.selectedStyleSheetIndex = m_DocumentSession.styleSheets.empty() ? -1 : 0;
	m_WorkspaceState.selectedSelectorIndex = -1;
	m_WorkspaceState.previewHover = false;
	m_NewSelectorBuffer[0] = '\0';
	QueuePreviewRefresh();


	if (m_StatusMessage.empty())
	{
		m_StatusMessage = "Loaded the selected UIComponent into the builder.";
	}

	SetVisible(true);
	return true;
}


void UIBuilderWindow::CreateNewDocumentInternal()
{
	m_StatusMessage.clear();
	BuilderDocumentSession nextSession;
	nextSession.entryContext = UIBuilderEntryContext::Unsaved;
	nextSession.root = CreateEmptyRoot();

	UIElement::Ptr container = UIElement::CreateDefault(UIElementType::VisualElement);
	container->id = "main-container";
	container->classList = { "container" };

	UIElement::Ptr title = UIElement::CreateDefault(UIElementType::Label);
	title->id = "title";
	title->text = "Hello TerraEngine UI";

	UIElement::Ptr button = UIElement::CreateDefault(UIElementType::Button);
	button->id = "play-button";
	button->text = "Play";

	container->AddChild(title);
	container->AddChild(button);
	nextSession.root->AddChild(container);

	nextSession.styleSheets.emplace_back();
	nextSession.styleSheets.front().attached = true;
	const auto appendRule = [&](std::string_view selectorText, const auto& configureRule) {
		UIStyleRule rule;

		if (!m_Parser.TryParseSelector(selectorText, rule))
		{
			return;
		}

		configureRule(rule.style);
		rule.order = nextSession.styleSheets.front().rules.size();
		nextSession.styleSheets.front().rules.push_back(std::move(rule));
	};

	appendRule(":root", [](UIStyle& style) {
		style.width = UIValue::Percent(100.0f);
		style.height = UIValue::Percent(100.0f);
	});
	appendRule("#main-container", [](UIStyle& style) {
		style.flexDirection = UIFlexDirection::Column;
		style.justifyContent = UIJustifyContent::Center;
		style.alignItems = UIAlignItems::Center;
		style.width = UIValue::Percent(100.0f);
		style.height = UIValue::Percent(100.0f);
		style.backgroundColor = glm::vec4(0.08f, 0.09f, 0.12f, 0.95f);
	});
	appendRule("#title", [](UIStyle& style) {
		style.fontSize = 28.0f;
		style.color = glm::vec4(0.96f, 0.96f, 0.98f, 1.0f);
		style.margin = UIEdgeValues::All(UIValue::Points(8.0f));
	});
	appendRule("#play-button", [](UIStyle& style) {
		style.width = UIValue::Points(180.0f);
		style.height = UIValue::Points(56.0f);
		style.backgroundColor = glm::vec4(0.16f, 0.44f, 0.78f, 1.0f);
		style.borderRadius = 8.0f;
		style.color = glm::vec4(1.0f);
	});
	appendRule("#play-button:hover", [](UIStyle& style) {
		style.backgroundColor = glm::vec4(0.26f, 0.56f, 0.92f, 1.0f);
		style.transform.scale = 1.05f;
	});

	m_Parser.ApplyStyles(nextSession.root, nextSession.styleSheets.front().rules);
	m_DocumentSession = std::move(nextSession);
	RefreshSessionDisplayNames();
	CaptureSessionRevisions();
	SetSelectedElement(container, UIBuilderPanelFocus::Hierarchy);
	m_WorkspaceState.selectedStyleSheetIndex = 0;
	m_WorkspaceState.selectedSelectorIndex = -1;
	m_WorkspaceState.previewHover = false;
	m_NewSelectorBuffer[0] = '\0';
	ClearPrompt();
	QueuePreviewRefresh();
	m_StatusMessage = "Created a new sample UI document. Double-click a .tuiml asset to edit a saved file.";
}


bool UIBuilderWindow::SaveWorkingDocument()
{
	if (!m_DocumentSession.root)
	{
		m_StatusMessage = "There is no working document to save.";
		return false;
	}

	std::filesystem::path markupPath = GetMarkupAssetPath();

	if (markupPath.empty())
	{
		m_StatusMessage = "Open a .tuiml asset from the Project Browser before saving.";
		return false;
	}

	const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();
	const std::filesystem::path resolvedMarkupPath = ResolveAuthoredPath(markupPath);
	std::filesystem::create_directories(resolvedMarkupPath.parent_path());
	std::vector<std::filesystem::path> savedStylePaths;
	std::size_t writtenStyleSheetCount = 0;

	for (StyleSheetSession& styleSession : m_DocumentSession.styleSheets)
	{
		if (styleSession.removeOnNextSave && !styleSession.internalPath.empty())
		{
			std::error_code errorCode;
			std::filesystem::remove(ResolveAuthoredPath(styleSession.internalPath), errorCode);
			styleSession.internalPath.clear();
			styleSession.removeOnNextSave = false;
			styleSession.attached = false;
		}

		const bool shouldWriteStyleSheet = styleSession.attached || !styleSession.rules.empty();

		if (!shouldWriteStyleSheet)
		{
			continue;
		}


		if (styleSession.internalPath.empty())
		{
			styleSession.internalPath = NormalizeToAssetRelativePath(assetRoot, SuggestNewStyleSheetPath());
		}

		const std::filesystem::path resolvedStylePath = ResolveAuthoredPath(styleSession.internalPath);
		std::filesystem::create_directories(resolvedStylePath.parent_path());

		std::ofstream styleOutput(resolvedStylePath);

		if (!styleOutput)
		{
			m_StatusMessage = "Failed to open the stylesheet file for writing.";
			return false;
		}

		styleOutput << m_Parser.SerializeStyles(styleSession.rules);
		styleOutput.close();

		savedStylePaths.push_back(styleSession.internalPath);
		++writtenStyleSheetCount;
	}

	m_DocumentSession.root->styleSheetSources.clear();

	for (const std::filesystem::path& savedStylePath : savedStylePaths)
	{
		m_DocumentSession.root->styleSheetSources.push_back(savedStylePath.generic_string());
	}

	const std::string serializedMarkup = UISystem::Instance().SerializeMarkup(m_DocumentSession.root);
	std::ofstream markupOutput(resolvedMarkupPath);

	if (!markupOutput)
	{
		m_StatusMessage = "Failed to open the markup file for writing.";
		return false;
	}

	markupOutput << serializedMarkup;
	markupOutput.close();


	if (m_DocumentSession.sceneEntity != entt::null)
	{
		auto& objectManager = ObjectManager::instance();
		if (std::optional<Object> object = objectManager.getObject(m_DocumentSession.sceneEntity))
		{

			if (UIComponent* component = objectManager.GetComponent<UIComponent>(*object))
			{
				// UIComponent now points only at the authored .tuiml asset. Any
				// attached .tuss files stay authored inside that document so the
				// builder remains the single stylesheet management surface.
				component->LoadMarkup(NormalizeToAssetRelativePath(assetRoot, resolvedMarkupPath));
				component->SetSourceStyle("");
				component->SetRoot(m_DocumentSession.root->Clone());
			}
		}
	}

	m_DocumentSession.internalPath = NormalizeToAssetRelativePath(assetRoot, resolvedMarkupPath);
	m_DocumentSession.dirty = false;

	for (StyleSheetSession& styleSession : m_DocumentSession.styleSheets)
	{
		styleSession.dirty = false;
	}
	m_DocumentSession.styleSheets.erase(
		std::remove_if(
			m_DocumentSession.styleSheets.begin(),
			m_DocumentSession.styleSheets.end(),
			[](const StyleSheetSession& styleSession) {
				return !styleSession.attached
					&& styleSession.rules.empty()
					&& styleSession.internalPath.empty()
					&& !styleSession.removeOnNextSave;
			}),
		m_DocumentSession.styleSheets.end());

	if (m_WorkspaceState.selectedStyleSheetIndex >= static_cast<int>(m_DocumentSession.styleSheets.size()))
	{
		m_WorkspaceState.selectedStyleSheetIndex = m_DocumentSession.styleSheets.empty()
			? -1
			: static_cast<int>(m_DocumentSession.styleSheets.size()) - 1;
	}
	RefreshSessionDisplayNames();
	CaptureSessionRevisions();
	QueuePreviewRefresh();

	m_StatusMessage = writtenStyleSheetCount > 0
		? "Saved " + resolvedMarkupPath.filename().string() + " and " + std::to_string(writtenStyleSheetCount) + " stylesheet(s)."
		: "Saved " + resolvedMarkupPath.filename().string() + ".";
	return true;
}


void UIBuilderWindow::DetachStyleSheetSession()
{
	StyleSheetSession* styleSession = GetSelectedStyleSheetSession();

	if (styleSession == nullptr)
	{
		m_StatusMessage = "Select a stylesheet to detach.";
		return;
	}

	styleSession->rules.clear();
	styleSession->attached = false;
	styleSession->dirty = false;
	styleSession->loadError.clear();
	styleSession->removeOnNextSave = !styleSession->internalPath.empty();
	SelectStyleRule(m_WorkspaceState.selectedStyleSheetIndex, -1);
	ApplyWorkingStyles();
	MarkDocumentDirty("Detached the stylesheet from the current UI document.");
	RefreshSessionDisplayNames();
}


void UIBuilderWindow::AttachSiblingStyleSheet()
{
	const std::filesystem::path markupPath = ResolveAuthoredPath(m_DocumentSession.internalPath);

	if (markupPath.empty())
	{
		m_StatusMessage = "Save or open a .tuiml asset before attaching a sibling stylesheet.";
		return;
	}

	const std::filesystem::path resolvedStylePath = InferStylePathFromMarkup(markupPath);

	if (!std::filesystem::exists(resolvedStylePath))
	{
		m_StatusMessage = "No sibling .tuss stylesheet was found next to the current .tuiml asset.";
		return;
	}
	AttachStyleSheetAsset(NormalizeToAssetRelativePath(ResourceManager::Instance().GetAssetRoot(), resolvedStylePath));
}


void UIBuilderWindow::RequestCreateStyleSheetAsset()
{
	const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();

	if (assetRoot.empty())
	{
		m_StatusMessage = "Open a project before creating a stylesheet asset.";
		return;
	}

	std::filesystem::path suggestedPath = SuggestNewStyleSheetPath();

	if (suggestedPath.empty())
	{
		// New documents do not have a saved .tuiml path yet, so fall back to a
		// unique name at the asset root instead of inventing a hidden path.
		std::filesystem::path candidate = assetRoot / "New StyleSheet.tuss";
		for (int suffixIndex = 2; std::filesystem::exists(candidate); ++suffixIndex)
		{
			candidate = assetRoot / ("New StyleSheet " + std::to_string(suffixIndex) + ".tuss");
		}

		suggestedPath = NormalizeToAssetRelativePath(assetRoot, candidate);
	}

	const std::filesystem::path resolvedSuggestedPath = ResolveAuthoredPath(suggestedPath);
	IGFD::FileDialogConfig config;
	config.path = resolvedSuggestedPath.parent_path().string();
	config.filePathName = resolvedSuggestedPath.string();
	config.flags = ImGuiFileDialogFlags_Modal
		| ImGuiFileDialogFlags_ConfirmOverwrite
		| ImGuiFileDialogFlags_HideColumnType;

	ImGuiFileDialog::Instance()->OpenDialog(
		kCreateStyleSheetDialogKey,
		"Create UI Stylesheet",
		".tuss",
		config);
}


void UIBuilderWindow::BeginPrompt(UIBuilderPendingPrompt kind, const std::filesystem::path& markupPath, entt::entity sceneEntity)
{
	m_PromptState.kind = kind;
	m_PromptState.targetMarkupPath = markupPath;
	m_PromptState.targetSceneEntity = sceneEntity;
	m_PromptState.markupExternallyChanged = false;
	m_PromptState.styleExternallyChanged = false;
	m_ShouldOpenPromptPopup = true;
}


void UIBuilderWindow::ClearPrompt()
{
	m_PromptState = {};
	m_ShouldOpenPromptPopup = false;
}


void UIBuilderWindow::ReloadCurrentSessionFromSource()
{
	SetVisible(true);
	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (m_DocumentSession.entryContext)
	{
	case UIBuilderEntryContext::AssetBrowser:

		if (!m_DocumentSession.internalPath.empty())
		{
			OpenMarkupDocumentInternal(m_DocumentSession.internalPath);
		}
		break;
	case UIBuilderEntryContext::SceneComponent:
		LoadSelectedComponentInternal(m_DocumentSession.sceneEntity);
		break;
	case UIBuilderEntryContext::Unsaved:
	default:
		CreateNewDocumentInternal();
		break;
	}
}


void UIBuilderWindow::RestoreCleanSessionAfterCloseDiscard()
{
	switch (m_DocumentSession.entryContext)
	{
	case UIBuilderEntryContext::AssetBrowser:

		if (!m_DocumentSession.internalPath.empty())
		{
			OpenMarkupDocumentInternal(m_DocumentSession.internalPath);
			return;
		}
		break;
	case UIBuilderEntryContext::SceneComponent:

		if (m_DocumentSession.sceneEntity != entt::null)
		{
			LoadSelectedComponentInternal(m_DocumentSession.sceneEntity);
			return;
		}
		break;
	case UIBuilderEntryContext::Unsaved:
	default:
		break;
	}

	CreateNewDocumentInternal();
}


void UIBuilderWindow::CheckForExternalChanges()
{
	if (m_PromptState.kind != UIBuilderPendingPrompt::None)
	{
		return;
	}

	// Revision stamps are captured after each successful save/load so the builder
	// can warn before a later disk change silently overwrites in-memory edits.
	const std::optional<std::filesystem::file_time_type> currentMarkupRevision = TryGetLastWriteTime(ResolveAuthoredPath(m_DocumentSession.internalPath));
	const bool markupChanged = m_DocumentSession.revisionStamp.has_value()
		&& (!currentMarkupRevision.has_value() || *currentMarkupRevision != *m_DocumentSession.revisionStamp);

	bool styleChanged = false;

	for (const StyleSheetSession& styleSession : m_DocumentSession.styleSheets)
	{
		const std::optional<std::filesystem::file_time_type> currentStyleRevision = TryGetLastWriteTime(ResolveAuthoredPath(styleSession.internalPath));
		const bool currentSheetChanged = styleSession.revisionStamp.has_value()
			&& (!currentStyleRevision.has_value() || *currentStyleRevision != *styleSession.revisionStamp);

		if (currentSheetChanged)
		{
			styleChanged = true;
			break;
		}
	}


	if (markupChanged || styleChanged)
	{
		m_PromptState.kind = UIBuilderPendingPrompt::ExternalReload;
		m_PromptState.markupExternallyChanged = markupChanged;
		m_PromptState.styleExternallyChanged = styleChanged;
		m_ShouldOpenPromptPopup = true;
	}
}


void UIBuilderWindow::RebuildPreviewIfNeeded(const ImVec2& previewSize)
{
	if (!m_DocumentSession.root)
	{
		m_PreviewBatch = {};
		m_PreviewRoot.reset();
		return;
	}

	const glm::vec2 viewportSize(previewSize.x, previewSize.y);

	if (!m_PreviewDirty
		&& std::abs(m_WorkspaceState.previewViewport.x - viewportSize.x) < 0.01f
		&& std::abs(m_WorkspaceState.previewViewport.y - viewportSize.y) < 0.01f)
	{
		return;
	}

	// The preview uses a cloned tree so layout and hover rendering can mutate the
	// cached preview document without disturbing the user's editable source tree.
	const UISystem::UIBuildResult previewResult = UISystem::Instance().BuildPreviewDocument(m_DocumentSession.root, viewportSize);
	m_PreviewRoot = previewResult.root;
	m_PreviewBatch = previewResult.batch;
	m_WorkspaceState.previewViewport = viewportSize;
	m_PreviewDirty = false;
}


void UIBuilderWindow::OnGui()
{
	if (ImGui::Button("Load Selected UI"))
	{
		RequestLoadSelectedComponent();
	}
	ImGui::SameLine();

	if (ImGui::Button("New Document"))
	{
		RequestNewDocument();
	}
	ImGui::SameLine();

	if (ImGui::Button("Save"))
	{
		SaveWorkingDocument();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Document: %s", GetMarkupDisplayName().c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("StyleSheets: %zu", m_DocumentSession.styleSheets.size());


	if (!m_StatusMessage.empty())
	{
		ImGui::TextWrapped("%s", m_StatusMessage.c_str());
	}


	if (!m_DocumentSession.root)
	{
		CreateNewDocumentInternal();
	}

	CheckForExternalChanges();
	SyncSelectedElementHover();

	if (m_DocumentSession.root && m_DocumentSession.root->AdvanceAnimationRecursive(ImGui::GetIO().DeltaTime))
	{
		QueuePreviewRefresh();
	}

	const ImVec2 available = ImGui::GetContentRegionAvail();
	const float leftWidth = 250.0f;
	const float rightWidth = 360.0f;
	const float previewWidth = std::max(1.0f, available.x - leftWidth - rightWidth - (ImGui::GetStyle().ItemSpacing.x * 2.0f));

	ImGui::BeginChild("UIBuilderLeftColumn", ImVec2(leftWidth, available.y), false);
	DrawLibraryPanel();
	ImGui::Separator();
	ImGui::BeginChild("UIBuilderHierarchyPanel", ImVec2(0.0f, 0.0f), true);
	ImGui::TextUnformatted("Hierarchy");
	ImGui::Separator();

	if (m_DocumentSession.root)
	{
		DrawHierarchyNode(m_DocumentSession.root);
	}
	ImGui::EndChild();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("UIBuilderPreviewColumn", ImVec2(previewWidth, available.y), false);
	DrawPreviewPanel();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("UIBuilderRightColumn", ImVec2(0.0f, available.y), false);
	ImGui::BeginChild("UIBuilderStyleSheetsPanel", ImVec2(0.0f, 230.0f), true);
	DrawStyleSheetPanel();
	ImGui::EndChild();
	ImGui::Separator();
	ImGui::BeginChild("UIBuilderInspectorPanel", ImVec2(0.0f, 0.0f), true);
	DrawInspector();
	ImGui::EndChild();
	ImGui::EndChild();

	DrawPendingPromptModal();


	if (m_ShouldHideWindowNextFrame)
	{
		m_ShouldHideWindowNextFrame = false;
		ImGui::CloseCurrentPopup();
		SetVisible(false);
	}
}


void UIBuilderWindow::DrawLibraryPanel()
{
	ImGui::TextUnformatted("Library");
	ImGui::Separator();

	int selectedCategory = static_cast<int>(m_WorkspaceState.libraryCategory);

	if (ImGui::Combo("Category", &selectedCategory, kLibraryCategoryLabels.data(), static_cast<int>(kLibraryCategoryLabels.size())))
	{
		m_WorkspaceState.libraryCategory = static_cast<UIBuilderLibraryCategory>(selectedCategory);
	}

	const auto canShowType = [&](UIElementType type) {
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (m_WorkspaceState.libraryCategory)
		{
		case UIBuilderLibraryCategory::All: return true;
		case UIBuilderLibraryCategory::Containers: return type == UIElementType::VisualElement;
		case UIBuilderLibraryCategory::Text: return type == UIElementType::Label;
		case UIBuilderLibraryCategory::Controls: return type == UIElementType::Button;
		case UIBuilderLibraryCategory::Media: return type == UIElementType::Image;
		default: return true;
		}
	};

	const auto addElementFromLibrary = [&](UIElementType type) {
		UIElement::Ptr parent = GetSelectedInsertionParent();

		if (!parent)
		{
			parent = m_DocumentSession.root;
		}


		if (!parent)
		{
			return;
		}

		UIElement::Ptr element = UIElement::CreateDefault(type);
		parent->AddChild(element);
		SetSelectedElement(element, UIBuilderPanelFocus::Library);
		ApplyWorkingStyles();
		MarkDocumentDirty("Added " + element->typeName + " from the Library.");
	};


	if (canShowType(UIElementType::VisualElement) && ImGui::Button("VisualElement", ImVec2(-1.0f, 0.0f)))
	{
		addElementFromLibrary(UIElementType::VisualElement);
	}

	if (canShowType(UIElementType::Label) && ImGui::Button("Label", ImVec2(-1.0f, 0.0f)))
	{
		addElementFromLibrary(UIElementType::Label);
	}

	if (canShowType(UIElementType::Button) && ImGui::Button("Button", ImVec2(-1.0f, 0.0f)))
	{
		addElementFromLibrary(UIElementType::Button);
	}

	if (canShowType(UIElementType::Image) && ImGui::Button("Image", ImVec2(-1.0f, 0.0f)))
	{
		addElementFromLibrary(UIElementType::Image);
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Selected parent: %s", GetSelectedInsertionParent() ? GetSelectedInsertionParent()->GetDisplayLabel().c_str() : "None");
}


void UIBuilderWindow::DrawHierarchyNode(const UIElement::Ptr& element)
{
	if (!element)
	{
		return;
	}

	const UIElement::Ptr selected = GetSelectedWorkingElement();
	const ImGuiTreeNodeFlags flags = (selected == element ? ImGuiTreeNodeFlags_Selected : 0)
		| (element->children.empty() ? ImGuiTreeNodeFlags_Leaf : 0)
		| ImGuiTreeNodeFlags_OpenOnArrow
		| ImGuiTreeNodeFlags_SpanAvailWidth;

	const bool opened = ImGui::TreeNodeEx(static_cast<void*>(element.get()), flags, "%s", element->GetDisplayLabel().c_str());

	if (ImGui::IsItemClicked())
	{
		SetSelectedElement(element, UIBuilderPanelFocus::Hierarchy);
	}


	if (ImGui::BeginDragDropSource())
	{
		const UIElement* payloadPointer = element.get();
		ImGui::SetDragDropPayload("UIBUILDER_ELEMENT", &payloadPointer, sizeof(payloadPointer));
		ImGui::TextUnformatted(element->GetDisplayLabel().c_str());
		ImGui::EndDragDropSource();
	}


	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UIBUILDER_ELEMENT"))
		{
			const UIElement* draggedPointer = *static_cast<const UIElement* const*>(payload->Data);

			if (UIElement::Ptr draggedElement = FindElementByPointer(m_DocumentSession.root, draggedPointer))
			{
				if (draggedElement != element && draggedElement->ReparentTo(element))
				{
					SetSelectedElement(draggedElement, UIBuilderPanelFocus::Hierarchy);
					ApplyWorkingStyles();
					MarkDocumentDirty("Reparented " + draggedElement->GetDisplayLabel() + " in the hierarchy.");
				}
			}
		}
		ImGui::EndDragDropTarget();
	}


	if (opened)
	{
		for (const UIElement::Ptr& child : element->children)
		{
			DrawHierarchyNode(child);
		}
		ImGui::TreePop();
	}
}


void UIBuilderWindow::DrawPreviewPanel()
{
	ImGui::TextUnformatted("Preview");
	ImGui::Separator();
	ImGui::Checkbox("Preview :hover", &m_WorkspaceState.previewHover);
	ImGui::Separator();

	const ImVec2 previewMin = ImGui::GetCursorScreenPos();
	ImVec2 previewSize = ImGui::GetContentRegionAvail();
	previewSize.x = std::max(1.0f, previewSize.x);
	previewSize.y = std::max(1.0f, previewSize.y);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(previewMin, ImVec2(previewMin.x + previewSize.x, previewMin.y + previewSize.y), IM_COL32(20, 20, 20, 255), 4.0f);
	drawList->PushClipRect(previewMin, ImVec2(previewMin.x + previewSize.x, previewMin.y + previewSize.y), true);

	for (float y = 0.0f; y < previewSize.y; y += 16.0f) {

		for (float x = 0.0f; x < previewSize.x; x += 16.0f) {

			if ((static_cast<int>(x / 16.0f) + static_cast<int>(y / 16.0f)) % 2 == 0) {
				ImVec2 pMin(previewMin.x + x, previewMin.y + y);
				ImVec2 pMax(pMin.x + 16.0f, pMin.y + 16.0f);
				drawList->AddRectFilled(pMin, pMax, IM_COL32(35, 35, 35, 255));
			}
		}
	}
	drawList->PopClipRect();

	RebuildPreviewIfNeeded(previewSize);
	DrawUIBatchPreview(m_PreviewBatch, drawList, previewMin, previewSize);


	if (UIElement::Ptr selected = GetSelectedWorkingElement())
	{
		std::vector<std::size_t> selectedPath;

		if (BuildElementPath(m_DocumentSession.root, selected.get(), selectedPath))
		{
			if (UIElement::Ptr previewSelected = ResolveElementAtPath(m_PreviewRoot, selectedPath))
			{
				const glm::vec2 elementMin = GetPreviewElementMin(*previewSelected);
				const glm::vec2 elementSize = GetPreviewElementSize(*previewSelected);
				const ImVec2 min(previewMin.x + elementMin.x, previewMin.y + elementMin.y);
				const ImVec2 max(min.x + elementSize.x, min.y + elementSize.y);
				drawList->AddRect(min, max, IM_COL32(84, 168, 255, 255), 4.0f, 2.0f, 0);
			}
		}
	}

	ImGui::InvisibleButton("UIBuilderPreviewCanvas", previewSize);

	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_PreviewRoot)
	{
		const ImVec2 mousePosition = ImGui::GetMousePos();
		const glm::vec2 localPoint(mousePosition.x - previewMin.x, mousePosition.y - previewMin.y);
		std::vector<std::size_t> currentPath;
		std::vector<std::size_t> hitPath;

		if (HitTestPreviewElement(m_PreviewRoot, localPoint, currentPath, hitPath))
		{
			if (UIElement::Ptr resolved = ResolveElementAtPath(m_DocumentSession.root, hitPath))
			{
				SetSelectedElement(resolved, UIBuilderPanelFocus::Preview);
			}
		}
	}
}


void UIBuilderWindow::DrawStyleSheetPanel()
{
	ImGui::Text("StyleSheets - %zu attached", m_DocumentSession.styleSheets.size());


	if (ImGui::Button("+"))
	{
		ImGui::OpenPopup("AddStyleSelectorPopup");
	}
	ImGui::SameLine();

	if (ImGui::Button("Create"))
	{
		RequestCreateStyleSheetAsset();
	}
	ImGui::SameLine();

	if (ImGui::Button("Attach Sibling"))
	{
		AttachSiblingStyleSheet();
	}
	ImGui::SameLine();

	if (ImGui::Button("Detach") && GetSelectedStyleSheetSession() != nullptr)
	{
		BeginPrompt(UIBuilderPendingPrompt::DetachStyleSheet);
	}


	if (const StyleSheetSession* selectedSession = GetSelectedStyleSheetSession())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("Active: %s", selectedSession->displayName.c_str());
	}


	if (ImGui::BeginPopup("AddStyleSelectorPopup"))
	{
		if (ImGui::MenuItem(":root")) { CreateStyleRule(":root"); }

		if (ImGui::MenuItem("VisualElement")) { CreateStyleRule("VisualElement"); }

		if (ImGui::MenuItem("Button")) { CreateStyleRule("Button"); }

		if (ImGui::MenuItem("Label")) { CreateStyleRule("Label"); }

		if (ImGui::MenuItem("Image")) { CreateStyleRule("Image"); }

		if (UIElement::Ptr selectedElement = GetSelectedWorkingElement())
		{
			ImGui::Separator();

			if (!selectedElement->id.empty() && ImGui::MenuItem(("#" + selectedElement->id).c_str()))
			{
				CreateStyleRule("#" + selectedElement->id);
			}

			if (!selectedElement->classList.empty() && ImGui::MenuItem(("." + selectedElement->classList.front()).c_str()))
			{
				CreateStyleRule("." + selectedElement->classList.front());
			}
		}
		ImGui::EndPopup();
	}

	ImGui::SetNextItemWidth(-1.0f);

	if (ImGui::InputTextWithHint("##NewSelector", "Add new selector...", m_NewSelectorBuffer, sizeof(m_NewSelectorBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		if (CreateStyleRule(m_NewSelectorBuffer))
		{
			m_NewSelectorBuffer[0] = '\0';
		}
	}


	if (ImGuiFileDialog::Instance()->Display(kCreateStyleSheetDialogKey))
	{
		if (ImGuiFileDialog::Instance()->IsOk())
		{
			const std::filesystem::path assetRoot = ResourceManager::Instance().GetAssetRoot();
			std::filesystem::path selectedPath = ImGuiFileDialog::Instance()->GetFilePathName(IGFD_ResultMode_AddIfNoFileExt);

			if (selectedPath.extension() != ".tuss")
			{
				selectedPath.replace_extension(".tuss");
			}


			if (!IsPathInsideRoot(assetRoot, selectedPath))
			{
				m_StatusMessage = "Create stylesheets inside the project's asset folder.";
			}
			else
			{
				const std::filesystem::path normalizedStylePath = NormalizeToAssetRelativePath(assetRoot, selectedPath);

				if (std::filesystem::exists(selectedPath))
				{
					AttachStyleSheetAsset(normalizedStylePath);
				}
				else
				{
					std::filesystem::create_directories(selectedPath.parent_path());
					std::ofstream styleOutput(selectedPath);

					if (!styleOutput)
					{
						m_StatusMessage = "Failed to create the stylesheet file.";
					}
					else
					{
						styleOutput << m_Parser.SerializeStyles(std::vector<UIStyleRule>{});
						styleOutput.close();


						if (AttachStyleSheetAsset(normalizedStylePath))
						{
							m_StatusMessage = "Created stylesheet: " + selectedPath.filename().string();
						}
					}
				}
			}
		}

		ImGuiFileDialog::Instance()->Close();
	}

	ImGui::BeginChild("StyleSheetSelectorList", ImVec2(0.0f, 150.0f), true);

	if (m_DocumentSession.styleSheets.empty())
	{
		ImGui::TextDisabled("No stylesheets attached. Create one, attach a sibling, or drag a .tuss asset here.");

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TUSS"))
			{
				IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
				const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);

				if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(assetGuid))
				{
					AttachStyleSheetAsset(meta->sourcePath);
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::EndChild();
		return;
	}


	for (std::size_t styleSheetIndex = 0; styleSheetIndex < m_DocumentSession.styleSheets.size(); ++styleSheetIndex)
	{
		StyleSheetSession& styleSession = m_DocumentSession.styleSheets[styleSheetIndex];
		ImGui::PushID(static_cast<int>(styleSheetIndex));
		ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

		if (m_WorkspaceState.selectedStyleSheetIndex == static_cast<int>(styleSheetIndex))
		{
			treeFlags |= ImGuiTreeNodeFlags_Selected;
		}

		const bool sheetOpen = ImGui::TreeNodeEx("##StyleSheetTree", treeFlags, "%s", styleSession.displayName.c_str());

		if (ImGui::IsItemClicked())
		{
			m_WorkspaceState.selectedStyleSheetIndex = static_cast<int>(styleSheetIndex);
			m_WorkspaceState.selectedSelectorIndex = -1;
			m_WorkspaceState.activePanel = UIBuilderPanelFocus::StyleSheets;
		}


		if (!styleSession.loadError.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", styleSession.loadError.c_str());
		}


		if (sheetOpen)
		{
			if (styleSession.rules.empty())
			{
				ImGui::TextDisabled("No selectors in this stylesheet yet.");
			}


			for (std::size_t index = 0; index < styleSession.rules.size(); ++index)
			{
				const UIStyleRule& rule = styleSession.rules[index];
				const bool isSelected = static_cast<int>(styleSheetIndex) == m_WorkspaceState.selectedStyleSheetIndex
					&& static_cast<int>(index) == m_WorkspaceState.selectedSelectorIndex;
				const ImVec4 ruleColor = rule.validity == UISelectorValidityState::Valid
					? ImVec4(0.35f, 0.85f, 0.35f, 1.0f)
					: ImVec4(1.0f, 0.75f, 0.35f, 1.0f);

				ImGui::PushStyleColor(ImGuiCol_Text, ruleColor);

				if (ImGui::Selectable(rule.selectorText.c_str(), isSelected))
				{
					SelectStyleRule(static_cast<int>(styleSheetIndex), static_cast<int>(index));
				}
				ImGui::PopStyleColor();


				if (ImGui::IsItemHovered() && !rule.validationMessage.empty())
				{
					ImGui::SetTooltip("%s", rule.validationMessage.c_str());
				}


				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem("Delete Selector"))
					{
						styleSession.rules.erase(styleSession.rules.begin() + static_cast<std::ptrdiff_t>(index));

						for (std::size_t reorderIndex = 0; reorderIndex < styleSession.rules.size(); ++reorderIndex)
						{
							styleSession.rules[reorderIndex].order = reorderIndex;
						}


						if (m_WorkspaceState.selectedStyleSheetIndex == static_cast<int>(styleSheetIndex)
							&& m_WorkspaceState.selectedSelectorIndex >= static_cast<int>(styleSession.rules.size()))
						{
							m_WorkspaceState.selectedSelectorIndex = static_cast<int>(styleSession.rules.size()) - 1;
						}
						ApplyWorkingStyles();
						MarkStyleSheetDirty("Removed a selector from the stylesheet.");
						ImGui::EndPopup();
						break;
					}
					ImGui::EndPopup();
				}
			}

			ImGui::TreePop();
		}

		ImGui::PopID();
	}


	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TUSS"))
		{
			IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
			const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);

			if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(assetGuid))
			{
				AttachStyleSheetAsset(meta->sourcePath);
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::EndChild();
}


bool UIBuilderWindow::DrawStyleEditor(UIStyle& style)
{
	bool changed = false;
	ImGui::PushID(&style);

	const auto unsetOptional = [&](const char* buttonId, auto& optionalValue) {
		ImGui::SameLine();

		if (ImGui::SmallButton(buttonId) && optionalValue.has_value())
		{
			optionalValue.reset();
			changed = true;
		}
	};

	float width = style.width.has_value() ? style.width->value : 0.0f;
	float height = style.height.has_value() ? style.height->value : 0.0f;
	float fontSize = style.fontSize.value_or(16.0f);
	float borderRadius = style.borderRadius.value_or(0.0f);
	float opacity = style.opacity.value_or(1.0f);
	float scale = style.transform.scale.value_or(1.0f);
	float rotation = style.transform.rotationDegrees.value_or(0.0f);
	glm::vec2 translate = style.transform.translate.value_or(glm::vec2(0.0f, 0.0f));
	glm::vec4 backgroundColor = style.backgroundColor.value_or(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
	glm::vec4 foregroundColor = style.color.value_or(glm::vec4(1.0f));
	int flexDirectionIndex = FlexDirectionToIndex(style.flexDirection);
	int justifyContentIndex = JustifyContentToIndex(style.justifyContent);
	int alignItemsIndex = AlignItemsToIndex(style.alignItems);


	if (ImGui::DragFloat("Width", &width, 1.0f, 0.0f, 4096.0f))
	{
		style.width = UIValue::Points(width);
		changed = true;
	}
	unsetOptional("Unset##Width", style.width);


	if (ImGui::DragFloat("Height", &height, 1.0f, 0.0f, 4096.0f))
	{
		style.height = UIValue::Points(height);
		changed = true;
	}
	unsetOptional("Unset##Height", style.height);


	if (ImGui::Combo("Flex Direction", &flexDirectionIndex, kFlexDirectionLabels.data(), static_cast<int>(kFlexDirectionLabels.size())))
	{
		if (flexDirectionIndex == 0)
		{
			style.flexDirection.reset();
		}
		else
		{
			style.flexDirection = flexDirectionIndex == 1 ? UIFlexDirection::Row : UIFlexDirection::Column;
		}
		changed = true;
	}

	if (ImGui::Combo("Justify Content", &justifyContentIndex, kJustifyContentLabels.data(), static_cast<int>(kJustifyContentLabels.size())))
	{
		switch (justifyContentIndex)
		{
		case 1: style.justifyContent = UIJustifyContent::FlexStart; break;
		case 2: style.justifyContent = UIJustifyContent::Center; break;
		case 3: style.justifyContent = UIJustifyContent::FlexEnd; break;
		case 4: style.justifyContent = UIJustifyContent::SpaceBetween; break;
		case 5: style.justifyContent = UIJustifyContent::SpaceAround; break;
		default: style.justifyContent.reset(); break;
		}
		changed = true;
	}

	if (ImGui::Combo("Align Items", &alignItemsIndex, kAlignItemsLabels.data(), static_cast<int>(kAlignItemsLabels.size())))
	{
		switch (alignItemsIndex)
		{
		case 1: style.alignItems = UIAlignItems::Stretch; break;
		case 2: style.alignItems = UIAlignItems::FlexStart; break;
		case 3: style.alignItems = UIAlignItems::Center; break;
		case 4: style.alignItems = UIAlignItems::FlexEnd; break;
		default: style.alignItems.reset(); break;
		}
		changed = true;
	}


	if (ImGui::DragFloat("Font Size", &fontSize, 1.0f, 8.0f, 128.0f))
	{
		style.fontSize = fontSize;
		changed = true;
	}
	unsetOptional("Unset##FontSize", style.fontSize);


	if (ImGui::DragFloat("Corner Radius", &borderRadius, 0.5f, 0.0f, 64.0f))
	{
		style.borderRadius = borderRadius;
		changed = true;
	}
	unsetOptional("Unset##CornerRadius", style.borderRadius);


	if (ImGui::DragFloat("Opacity", &opacity, 0.01f, 0.0f, 1.0f))
	{
		style.opacity = opacity;
		changed = true;
	}
	unsetOptional("Unset##Opacity", style.opacity);


	if (ImGui::ColorEdit4("Background", &backgroundColor.x))
	{
		style.backgroundColor = backgroundColor;
		changed = true;
	}
	unsetOptional("Unset##Background", style.backgroundColor);


	if (ImGui::ColorEdit4("Foreground", &foregroundColor.x))
	{
		style.color = foregroundColor;
		changed = true;
	}
	unsetOptional("Unset##Foreground", style.color);


	if (ImGui::DragFloat2("Translate", &translate.x, 1.0f, -4096.0f, 4096.0f))
	{
		style.transform.translate = translate;
		changed = true;
	}
	ImGui::SameLine();

	if (ImGui::SmallButton("Unset##Translate") && style.transform.translate.has_value())
	{
		style.transform.translate.reset();
		changed = true;
	}


	if (ImGui::DragFloat("Scale", &scale, 0.01f, 0.01f, 16.0f))
	{
		style.transform.scale = scale;
		changed = true;
	}
	ImGui::SameLine();

	if (ImGui::SmallButton("Unset##Scale") && style.transform.scale.has_value())
	{
		style.transform.scale.reset();
		changed = true;
	}


	if (ImGui::DragFloat("Rotation", &rotation, 1.0f, -360.0f, 360.0f))
	{
		style.transform.rotationDegrees = rotation;
		changed = true;
	}
	ImGui::SameLine();

	if (ImGui::SmallButton("Unset##Rotation") && style.transform.rotationDegrees.has_value())
	{
		style.transform.rotationDegrees.reset();
		changed = true;
	}

	ImGui::PopID();
	return changed;
}


void UIBuilderWindow::DrawElementInspector(const UIElement::Ptr& selected)
{
	ImGui::Text("Inspector - %s", selected->GetDisplayLabel().c_str());
	ImGui::Separator();

	char idBuffer[128];
	char classBuffer[256];
	char textBuffer[256];
	char sourceBuffer[256];
	CopyStringToBuffer(selected->id, idBuffer, sizeof(idBuffer));
	CopyStringToBuffer(selected->GetClassListAsString(), classBuffer, sizeof(classBuffer));
	CopyStringToBuffer(selected->text, textBuffer, sizeof(textBuffer));
	CopyStringToBuffer(selected->source, sourceBuffer, sizeof(sourceBuffer));

	ImGui::TextUnformatted("Attributes");
	ImGui::Separator();


	if (ImGui::InputText("Name", idBuffer, sizeof(idBuffer)))
	{
		selected->id = idBuffer;
		ApplyWorkingStyles();
		selected->MarkDirty();
		MarkDocumentDirty("Updated the selected element name.");
	}

	if (ImGui::InputText("Text", textBuffer, sizeof(textBuffer)))
	{
		selected->text = textBuffer;
		selected->MarkDirty();
		MarkDocumentDirty("Updated the selected element text.");
	}


	if (selected->type == UIElementType::Image)
	{
		if (ImGui::InputText("Image Source", sourceBuffer, sizeof(sourceBuffer)))
		{
			selected->source = sourceBuffer;
			selected->MarkDirty();
			MarkDocumentDirty("Updated the selected image source.");
		}


		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE"))
			{
				IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
				const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);

				if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(assetGuid))
				{
					selected->source = meta->sourcePath.filename().string();
					selected->MarkDirty();
					MarkDocumentDirty("Dropped texture " + selected->source + " onto image source.");
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	ImGui::Spacing();
	ImGui::TextUnformatted("Element Actions");
	ImGui::Separator();


	if (selected->parent != nullptr && ImGui::Button("Duplicate"))
	{
		UIElement::Ptr clone = selected->Clone();
		UIElement::Ptr parent = selected->parent->shared_from_this();
		const auto iterator = std::find(parent->children.begin(), parent->children.end(), selected);
		const std::size_t insertIndex = iterator == parent->children.end()
			? parent->children.size()
			: static_cast<std::size_t>(std::distance(parent->children.begin(), iterator) + 1);
		parent->InsertChild(insertIndex, clone);

		SetSelectedElement(clone, UIBuilderPanelFocus::Inspector);
		ApplyWorkingStyles();
		MarkDocumentDirty("Duplicated the selected element.");
	}


	if (selected->parent != nullptr)
	{
		ImGui::SameLine();

		if (ImGui::Button("Remove"))
		{
			UIElement::Ptr parent = selected->parent->shared_from_this();
			parent->RemoveChild(selected);
			SetSelectedElement(parent, UIBuilderPanelFocus::Inspector);
			ApplyWorkingStyles();
			MarkDocumentDirty("Removed the selected element.");
			return;
		}

		ImGui::SameLine();

		if (ImGui::Button("Move Up"))
		{
			UIElement::Ptr parent = selected->parent->shared_from_this();
			const auto iterator = std::find(parent->children.begin(), parent->children.end(), selected);

			if (iterator != parent->children.begin() && iterator != parent->children.end())
			{
				const std::size_t index = static_cast<std::size_t>(std::distance(parent->children.begin(), iterator));
				parent->MoveChild(index, index - 1);
				ApplyWorkingStyles();
				MarkDocumentDirty("Moved the selected element up in the hierarchy.");
			}
		}

		ImGui::SameLine();

		if (ImGui::Button("Move Down"))
		{
			UIElement::Ptr parent = selected->parent->shared_from_this();
			const auto iterator = std::find(parent->children.begin(), parent->children.end(), selected);

			if (iterator != parent->children.end() && (iterator + 1) != parent->children.end())
			{
				const std::size_t index = static_cast<std::size_t>(std::distance(parent->children.begin(), iterator));
				parent->MoveChild(index, index + 1);
				ApplyWorkingStyles();
				MarkDocumentDirty("Moved the selected element down in the hierarchy.");
			}
		}


		if (selected->parent->parent != nullptr && ImGui::Button("Promote To Parent"))
		{
			UIElement::Ptr newParent = selected->parent->parent->shared_from_this();

			if (selected->ReparentTo(newParent))
			{
				ApplyWorkingStyles();
				MarkDocumentDirty("Promoted the selected element to its parent level.");
			}
		}
	}

	ImGui::Spacing();
	ImGui::TextUnformatted("StyleSheets");
	ImGui::Separator();

	ImGui::TextDisabled("Active StyleSheet: %s", GetStyleSheetDisplayName().c_str());

	if (ImGui::InputTextWithHint("Style Class List", "Add style classes...", classBuffer, sizeof(classBuffer)))
	{
		selected->SetClassListFromString(classBuffer);
		ApplyWorkingStyles();
		selected->MarkDirty();
		MarkDocumentDirty("Updated the selected element style classes.");
	}

	const auto findClassSelectorIndex = [&](std::string_view className) -> MatchingStyleRuleReference {

		for (std::size_t styleSheetIndex = 0; styleSheetIndex < m_DocumentSession.styleSheets.size(); ++styleSheetIndex)
		{
			const StyleSheetSession& styleSession = m_DocumentSession.styleSheets[styleSheetIndex];

			for (std::size_t ruleIndex = 0; ruleIndex < styleSession.rules.size(); ++ruleIndex)
			{
				const UIStyleRule& rule = styleSession.rules[ruleIndex];

				if (rule.validity == UISelectorValidityState::Valid
					&& rule.selectorKind == UISelectorKind::Class
					&& rule.pseudoState == UIPseudoState::None
					&& rule.selectorValue == className)
				{
					return MatchingStyleRuleReference{
						static_cast<int>(styleSheetIndex),
						static_cast<int>(ruleIndex),
					};
				}
			}
		}

		return {};
	};


	if (!selected->classList.empty())
	{
		ImGui::TextDisabled("Style Class Labels");
		// Unity exposes class labels directly from the element inspector so the
		// author can jump into the owning selector instead of editing a stale one.

		for (std::size_t classIndex = 0; classIndex < selected->classList.size(); ++classIndex)
		{
			if (classIndex > 0)
			{
				ImGui::SameLine();
			}

			const std::string classLabel = "." + selected->classList[classIndex];
			const MatchingStyleRuleReference classSelectorIndex = findClassSelectorIndex(selected->classList[classIndex]);
			const bool classHasSelector = classSelectorIndex.styleSheetIndex >= 0 && classSelectorIndex.ruleIndex >= 0;
			ImGui::PushStyleColor(ImGuiCol_Button, classHasSelector ? ImVec4(0.79f, 0.67f, 0.19f, 0.85f) : ImVec4(0.52f, 0.44f, 0.16f, 0.45f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, classHasSelector ? ImVec4(0.86f, 0.73f, 0.24f, 0.95f) : ImVec4(0.57f, 0.49f, 0.20f, 0.6f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, classHasSelector ? ImVec4(0.9f, 0.77f, 0.27f, 1.0f) : ImVec4(0.62f, 0.54f, 0.24f, 0.75f));
			ImGui::SmallButton(classLabel.c_str());
			ImGui::PopStyleColor(3);


			if (ImGui::IsItemHovered())
			{
				if (classHasSelector)
				{
					ImGui::SetTooltip("Double-click to jump to the %s selector.", classLabel.c_str());
				}
				else
				{
					ImGui::SetTooltip("Double-click to create the %s selector in the active stylesheet.", classLabel.c_str());
				}
			}


			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (classHasSelector)
				{
					SelectStyleRule(classSelectorIndex.styleSheetIndex, classSelectorIndex.ruleIndex);
				}

				else if (CreateStyleRule(classLabel))
				{
					m_StatusMessage = "Created selector " + classLabel + " from the selected element class.";
				}
			}
		}
	}
	else
	{
		ImGui::TextDisabled("No style classes are assigned to this element yet.");
	}

	const std::vector<MatchingStyleRuleReference> matchingRuleIndices = CollectMatchingRuleIndices(*selected);
	ImGui::TextDisabled("Matching Selectors");

	if (matchingRuleIndices.empty())
	{
		ImGui::TextDisabled("No selectors from the attached stylesheets currently match this element.");
	}
	else
	{

		for (const MatchingStyleRuleReference& matchingRule : matchingRuleIndices)
		{
			const StyleSheetSession& styleSession = m_DocumentSession.styleSheets[static_cast<std::size_t>(matchingRule.styleSheetIndex)];
			const UIStyleRule& rule = styleSession.rules[static_cast<std::size_t>(matchingRule.ruleIndex)];
			const bool isSelectedRule = m_WorkspaceState.activePanel == UIBuilderPanelFocus::StyleSheets
				&& matchingRule.styleSheetIndex == m_WorkspaceState.selectedStyleSheetIndex
				&& matchingRule.ruleIndex == m_WorkspaceState.selectedSelectorIndex;
			const std::string selectorLabel = styleSession.displayName + " / " + rule.selectorText;

			if (ImGui::Selectable(selectorLabel.c_str(), isSelectedRule))
			{
				SelectStyleRule(matchingRule.styleSheetIndex, matchingRule.ruleIndex);
			}
		}
	}

	ImGui::Spacing();
	ImGui::TextUnformatted("Inlined Styles");
	ImGui::Separator();
	// Element selections edit inline styles so stylesheet changes only happen
	// after the user explicitly selects a USS selector.

	if (DrawStyleEditor(selected->inlineStyle))
	{
		ApplyWorkingStyles();
		selected->MarkDirty();
		MarkDocumentDirty("Updated inline style properties for the selected element.");
	}
}


void UIBuilderWindow::DrawSelectorInspector(UIStyleRule& selectedRule)
{
	ImGui::Text("Inspector - %s", selectedRule.selectorText.c_str());
	ImGui::Separator();

	ImGui::TextUnformatted("Style Selector");
	ImGui::Separator();

	char selectorBuffer[128];
	CopyStringToBuffer(selectedRule.selectorText, selectorBuffer, sizeof(selectorBuffer));

	if (selectedRule.validity != UISelectorValidityState::Valid && !selectedRule.validationMessage.empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", selectedRule.validationMessage.c_str());
	}


	if (ImGui::InputText("Selector", selectorBuffer, sizeof(selectorBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		UIStyleRule parsedSelector;

		if (m_Parser.TryParseSelector(selectorBuffer, parsedSelector))
		{
			parsedSelector.style = selectedRule.style;
			parsedSelector.order = selectedRule.order;
			selectedRule = std::move(parsedSelector);
			ApplyWorkingStyles();
			MarkStyleSheetDirty("Updated the selector definition.");
		}
		else
		{
			selectedRule.selectorText = selectorBuffer;
			selectedRule.validity = UISelectorValidityState::RecoverableError;
			selectedRule.validationMessage = "Unsupported selector. Use :root, element names, #ids, .classes, and optional :hover.";
			MarkStyleSheetDirty("Updated the selector text, but it still needs attention.");
		}
	}

	ImGui::Spacing();
	ImGui::TextUnformatted("Styles");
	ImGui::Separator();


	if (DrawStyleEditor(selectedRule.style))
	{
		ApplyWorkingStyles();
		MarkStyleSheetDirty("Updated stylesheet properties for the selected selector.");
	}
}


void UIBuilderWindow::DrawInspector()
{
	if (m_WorkspaceState.activePanel == UIBuilderPanelFocus::StyleSheets)
	{

		if (UIStyleRule* selectedRule = GetSelectedStyleRule())
		{
			DrawSelectorInspector(*selectedRule);
			return;
		}
	}

	UIElement::Ptr selected = GetSelectedWorkingElement();

	if (!selected)
	{
		ImGui::TextUnformatted("Select a UI element from the hierarchy or preview.");
		return;
	}

	DrawElementInspector(selected);
}


void UIBuilderWindow::SyncSelectedElementHover()
{
	if (!m_DocumentSession.root)
	{
		return;
	}

	const auto clearRecursive = [](const auto& self, const UIElement::Ptr& element) -> void {

		if (!element)
		{
			return;
		}

		element->SetHoverState(false);

		for (const UIElement::Ptr& child : element->children)
		{
			self(self, child);
		}
	};
	clearRecursive(clearRecursive, m_DocumentSession.root);


	if (m_WorkspaceState.previewHover)
	{
		if (UIElement::Ptr selected = GetSelectedWorkingElement())
		{
			selected->SetHoverState(true);
			QueuePreviewRefresh();
		}
	}
}


void UIBuilderWindow::DrawPendingPromptModal()
{
	if (m_ShouldOpenPromptPopup)
	{
		ImGui::OpenPopup("UI Builder Pending Action");
		m_ShouldOpenPromptPopup = false;
	}


	if (!ImGui::BeginPopupModal("UI Builder Pending Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (m_PromptState.kind)
	{
	case UIBuilderPendingPrompt::CloseWindow:
	case UIBuilderPendingPrompt::NewDocument:
	case UIBuilderPendingPrompt::OpenMarkupAsset:
	case UIBuilderPendingPrompt::LoadSelectedComponent:
		ImGui::TextWrapped("This action would discard unsaved UI Builder changes.");
		ImGui::Separator();

		if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
		{
			if (SaveWorkingDocument())
			{
				ImGui::CloseCurrentPopup();
				const UIBuilderPendingPrompt resolvedKind = m_PromptState.kind;
				const std::filesystem::path pendingMarkupPath = m_PromptState.targetMarkupPath;
				const entt::entity pendingSceneEntity = m_PromptState.targetSceneEntity;
				ClearPrompt();

				// Dispatch by explicit engine state so each case documents one supported behavior.
				switch (resolvedKind)
				{
				case UIBuilderPendingPrompt::CloseWindow:
					m_ShouldHideWindowNextFrame = true;
					break;
				case UIBuilderPendingPrompt::NewDocument:
					CreateNewDocumentInternal();
					break;
				case UIBuilderPendingPrompt::OpenMarkupAsset:
					OpenMarkupDocumentInternal(pendingMarkupPath);
					break;
				case UIBuilderPendingPrompt::LoadSelectedComponent:
					LoadSelectedComponentInternal(pendingSceneEntity);
					break;
				default:
					break;
				}
			}
		}
		ImGui::SameLine();

		if (ImGui::Button("Discard", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			const UIBuilderPendingPrompt resolvedKind = m_PromptState.kind;
			const std::filesystem::path pendingMarkupPath = m_PromptState.targetMarkupPath;
			const entt::entity pendingSceneEntity = m_PromptState.targetSceneEntity;
			ClearPrompt();

			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (resolvedKind)
			{
			case UIBuilderPendingPrompt::CloseWindow:
				RestoreCleanSessionAfterCloseDiscard();
				m_ShouldHideWindowNextFrame = true;
				break;
			case UIBuilderPendingPrompt::NewDocument:
				CreateNewDocumentInternal();
				break;
			case UIBuilderPendingPrompt::OpenMarkupAsset:
				OpenMarkupDocumentInternal(pendingMarkupPath);
				break;
			case UIBuilderPendingPrompt::LoadSelectedComponent:
				LoadSelectedComponentInternal(pendingSceneEntity);
				break;
			default:
				break;
			}
		}
		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			CaptureSessionRevisions();
			ClearPrompt();
		}
		break;

	case UIBuilderPendingPrompt::ExternalReload:
		ImGui::TextWrapped("The current UI document changed on disk while the builder was open.");

		if (m_PromptState.markupExternallyChanged) { ImGui::BulletText("The .tuiml document changed externally."); }

		if (m_PromptState.styleExternallyChanged) { ImGui::BulletText("One or more attached .tuss stylesheets changed externally."); }
		ImGui::Separator();
		if (ImGui::Button("Reload", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			ClearPrompt();
			ReloadCurrentSessionFromSource();
		}
		ImGui::SameLine();

		if (ImGui::Button("Keep Current", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			CaptureSessionRevisions();
			m_StatusMessage = "Kept the current in-memory session after detecting external asset changes.";
			ClearPrompt();
		}
		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			ClearPrompt();
		}
		break;

	case UIBuilderPendingPrompt::DetachStyleSheet:
		ImGui::TextWrapped("Detach the current stylesheet from this document?");
		ImGui::TextWrapped("The detached stylesheet reference will be removed on the next save so reopened .tuiml assets stay detached.");
		ImGui::Separator();

		if (ImGui::Button("Detach", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			DetachStyleSheetSession();
			ClearPrompt();
		}
		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			ClearPrompt();
		}
		break;

	case UIBuilderPendingPrompt::None:
	default:
		ImGui::CloseCurrentPopup();
		ClearPrompt();
		break;
	}

	ImGui::EndPopup();
}
