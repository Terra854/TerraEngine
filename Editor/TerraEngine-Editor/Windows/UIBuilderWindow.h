// UIBuilderWindow.h — UI Builder Window Definition
//
// Defines the UIBuilderWindow class, a specialized editor for creating and layouting in-game user interfaces.
// It provides a visual canvas for arranging UI elements and configuring their styling and behavior.

#pragma once

#include "../Framework/EditorWindow.h"
#include "Engine/UI/UIElement.h"
#include "Engine/UI/UIParser.h"
#include "Engine/UI/UIRenderer.h"

#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class MainEditorSubsystem;

// UIBuilderEntryContext names the supported states explicitly so serialization and switches stay readable.
enum class UIBuilderEntryContext
{
	Unsaved,
	AssetBrowser,
	SceneComponent,
};

// UIBuilderPanelFocus names the supported states explicitly so serialization and switches stay readable.
enum class UIBuilderPanelFocus
{
	Library,
	Hierarchy,
	Preview,
	Inspector,
	StyleSheets,
};

// UIBuilderPendingPrompt names the supported states explicitly so serialization and switches stay readable.
enum class UIBuilderPendingPrompt
{
	None,
	CloseWindow,
	NewDocument,
	OpenMarkupAsset,
	LoadSelectedComponent,
	ExternalReload,
	DetachStyleSheet,
};

// UIBuilderLibraryCategory names the supported states explicitly so serialization and switches stay readable.
enum class UIBuilderLibraryCategory
{
	All,
	Containers,
	Text,
	Controls,
	Media,
};

// struct StyleSheetSession groups related state and operations so callers have one documented contract.
struct StyleSheetSession
{
	std::filesystem::path internalPath;
	std::string displayName = "None";
	std::vector<UIStyleRule> rules;
	bool attached = false;
	bool dirty = false;
	bool removeOnNextSave = false;
	std::optional<std::filesystem::file_time_type> revisionStamp;
	std::string loadError;
};

// struct BuilderDocumentSession groups related state and operations so callers have one documented contract.
struct BuilderDocumentSession
{
	std::filesystem::path internalPath;
	std::string displayName = "Unsaved UI";
	UIBuilderEntryContext entryContext = UIBuilderEntryContext::Unsaved;
	UIElement::Ptr root;
	entt::entity sceneEntity = entt::null;
	bool dirty = false;
	std::optional<std::filesystem::file_time_type> revisionStamp;
	std::vector<StyleSheetSession> styleSheets;
};

// struct BuilderWorkspaceState groups related state and operations so callers have one documented contract.
struct BuilderWorkspaceState
{
	std::weak_ptr<UIElement> selectedElement;
	int selectedStyleSheetIndex = -1;
	int selectedSelectorIndex = -1;
	UIBuilderPanelFocus activePanel = UIBuilderPanelFocus::Hierarchy;
	UIBuilderLibraryCategory libraryCategory = UIBuilderLibraryCategory::All;
	glm::vec2 previewViewport = glm::vec2(0.0f, 0.0f);
	bool previewHover = false;
};

// struct UIBuilderPromptState groups related state and operations so callers have one documented contract.
struct UIBuilderPromptState
{
	UIBuilderPendingPrompt kind = UIBuilderPendingPrompt::None;
	std::filesystem::path targetMarkupPath;
	entt::entity targetSceneEntity = entt::null;
	bool markupExternallyChanged = false;
	bool styleExternallyChanged = false;
};

// struct MatchingStyleRuleReference groups related state and operations so callers have one documented contract.
struct MatchingStyleRuleReference
{
	int styleSheetIndex = -1;
	int ruleIndex = -1;
};

// UIBuilderWindow provides a visual authoring surface for the runtime UI
// assets while keeping document state, stylesheet state, and recovery prompts
// explicit instead of scattering that state through raw path buffers.
class UIBuilderWindow : public EditorWindow
{
public:
	explicit UIBuilderWindow(MainEditorSubsystem* owner);
	void OpenMarkupDocument(const std::filesystem::path& markupPath);
	void ResetForProjectOpen();

protected:
	void OnGui() override;
	bool OnCloseRequested() override;

private:
	void RequestNewDocument();
	void RequestLoadSelectedComponent();
	bool OpenMarkupDocumentInternal(const std::filesystem::path& markupPath);
	bool LoadSelectedComponentInternal(entt::entity targetEntity = entt::null);
	void CreateNewDocumentInternal();
	bool SaveWorkingDocument();
	void DrawLibraryPanel();
	void DrawHierarchyNode(const UIElement::Ptr& element);
	void DrawPreviewPanel();
	void DrawStyleSheetPanel();
	void DrawInspector();
	void DrawPendingPromptModal();
	void SyncSelectedElementHover();
	void ApplyWorkingStyles();
	void QueuePreviewRefresh();
	void RebuildPreviewIfNeeded(const ImVec2& previewSize);
	void RefreshSessionDisplayNames();
	void CaptureSessionRevisions();
	void MarkDocumentDirty(std::string_view reason = {});
	void MarkStyleSheetDirty(std::string_view reason = {});
	void DetachStyleSheetSession();
	void AttachSiblingStyleSheet();
	void RequestCreateStyleSheetAsset();
	void BeginPrompt(UIBuilderPendingPrompt kind, const std::filesystem::path& markupPath = {}, entt::entity sceneEntity = entt::null);
	void ClearPrompt();
	void ReloadCurrentSessionFromSource();
	void RestoreCleanSessionAfterCloseDiscard();
	void CheckForExternalChanges();
	bool CreateStyleRule(std::string_view selectorText, bool selectNewRule = true);
	void SelectStyleRule(int styleSheetIndex, int styleRuleIndex, bool focusStyleSheet = true);
	void SetSelectedElement(const UIElement::Ptr& element, UIBuilderPanelFocus focus);
	[[nodiscard]] std::vector<MatchingStyleRuleReference> CollectMatchingRuleIndices(const UIElement& element) const;
	bool DrawStyleEditor(UIStyle& style);
	void DrawElementInspector(const UIElement::Ptr& selected);
	void DrawSelectorInspector(UIStyleRule& selectedRule);
	[[nodiscard]] int EnsureSelectedStyleSheetSession();
	[[nodiscard]] StyleSheetSession* GetSelectedStyleSheetSession();
	[[nodiscard]] const StyleSheetSession* GetSelectedStyleSheetSession() const;
	[[nodiscard]] std::vector<UIStyleRule> BuildCombinedStyleRules() const;
	[[nodiscard]] std::filesystem::path SuggestNewStyleSheetPath() const;
	bool AttachStyleSheetAsset(const std::filesystem::path& stylePath, bool selectAttachedSheet = true);
	[[nodiscard]] UIStyleRule* GetSelectedStyleRule();
	[[nodiscard]] const UIStyleRule* GetSelectedStyleRule() const;
	[[nodiscard]] bool HasUnsavedChanges() const;
	[[nodiscard]] std::filesystem::path GetMarkupAssetPath() const;
	[[nodiscard]] std::filesystem::path GetStyleAssetPath() const;
	[[nodiscard]] std::filesystem::path ResolveAuthoredPath(const std::filesystem::path& authoredPath) const;
	[[nodiscard]] std::string GetMarkupDisplayName() const;
	[[nodiscard]] std::string GetStyleSheetDisplayName() const;
	[[nodiscard]] UIElement::Ptr GetSelectedWorkingElement() const;
	[[nodiscard]] UIElement::Ptr GetSelectedInsertionParent() const;

	MainEditorSubsystem* m_Owner = nullptr;
	UIParser m_Parser;
	BuilderDocumentSession m_DocumentSession;
	BuilderWorkspaceState m_WorkspaceState;
	UIBuilderPromptState m_PromptState;
	UIRenderBatch m_PreviewBatch;
	UIElement::Ptr m_PreviewRoot;
	bool m_PreviewDirty = true;
	bool m_ShouldOpenPromptPopup = false;
	bool m_ShouldHideWindowNextFrame = false;
	std::string m_StatusMessage;
	char m_NewSelectorBuffer[128]{};
};
