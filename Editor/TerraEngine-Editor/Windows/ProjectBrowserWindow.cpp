// ProjectBrowserWindow.cpp — Project Browser Window Implementation
//
// Implements the asset management UI, including folder navigation, file searching, and thumbnail rendering.
// It interacts with the operating system's filesystem and the engine's resource management systems to handle asset lifecycle.

#include "ProjectBrowserWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "UIBuilderWindow.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "Engine/Resource/ResourceManager.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>
#include "../../../Engine/File Formats/TerraEngineMaterialFile.h"

namespace
{
const char* kDefaultUIMarkup = R"(<UIRoot>
	<VisualElement id="main-container" class="container">
		<Label id="title" text="New TerraEngine UI" />
		<Button id="primary-button" text="Click Me" />
	</VisualElement>
</UIRoot>
)";

// MakeUniqueAssetPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path MakeUniqueAssetPath(const std::filesystem::path& directory, const std::string& baseName, const std::string& extension)
{
	std::filesystem::path candidatePath = directory / (baseName + extension);
	int duplicateIndex = 1;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (std::filesystem::exists(candidatePath))
	{
		candidatePath = directory / (baseName + " (" + std::to_string(duplicateIndex++) + ")" + extension);
	}

	return candidatePath;
}

// OpenMarkupAssetInBuilder is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void OpenMarkupAssetInBuilder(MainEditorSubsystem* owner, const std::filesystem::path& markupPath)
{
	if (owner == nullptr)
	{
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::shared_ptr<UIBuilderWindow> uiBuilder = owner->GetWindow<UIBuilderWindow>())
	{
		uiBuilder->OpenMarkupDocument(std::filesystem::absolute(markupPath));
	}
}
}

ProjectBrowserWindow::ProjectBrowserWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Project Browser"), m_Owner(owner) {
	std::fill(m_NewAssetName.begin(), m_NewAssetName.end(), '\0');
	std::fill(m_NewScriptName.begin(), m_NewScriptName.end(), '\0');
}

// ProjectBrowserWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::OnGui() {
	if (ImGuiFileDialog::Instance()->Display("ImportAssetDlg")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::filesystem::path filePath = ImGuiFileDialog::Instance()->GetFilePathName();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(filePath)) {
				std::filesystem::path dest = AssetDatabase::Instance().GetAssetRoot() / filePath.filename();
				try {
					std::filesystem::copy(filePath, dest, std::filesystem::copy_options::overwrite_existing);
				} catch (const std::exception& e) {
					// Log error
				}
			}
		}
		ImGuiFileDialog::Instance()->Close();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Owner->GetContext() && m_Owner->GetContext()->projectManager->HasActiveProject()) {
		std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
		if (m_CurrentDirectory.empty() || !std::filesystem::exists(m_CurrentDirectory)) {
			m_CurrentDirectory = root;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Add")) {
			ImGui::OpenPopup("AddAssetPopup");
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopup("AddAssetPopup")) {
			if (ImGui::MenuItem("Folder")) {
				OpenCreateAssetModal("Create Folder", "New Folder", "");
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Material (.tmat)")) {
				OpenCreateAssetModal("Create Material", "New Material", ".tmat");
			}
			if (ImGui::MenuItem("Prefab (.teprefab)")) {
				OpenCreateAssetModal("Create Prefab", "New Prefab", ".teprefab");
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("UI Markup (.tuiml)")) {
				OpenCreateAssetModal("Create UI Markup", "New UI", ".tuiml");
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("C# Script (.cs)")) {
				std::fill(m_NewScriptName.begin(), m_NewScriptName.end(), '\0');
				const char* defaultName = "NewScript";
				std::copy(defaultName, defaultName + std::strlen(defaultName), m_NewScriptName.begin());
				m_CreateScriptError.clear();
				m_ShowCreateScriptModal = true;
			}
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		char searchBuf[128];
		strncpy(searchBuf, m_SearchFilter.c_str(), sizeof(searchBuf));
		ImGui::SetNextItemWidth(200);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::InputTextWithHint("##Search", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf))) {
			m_SearchFilter = searchBuf;
		}

		ImGui::Separator();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginTable("BrowserSplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthFixed, 200.0f);
			ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
			
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(root)) {
				ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
				if (root == m_CurrentDirectory) rootFlags |= ImGuiTreeNodeFlags_Selected;
				
				bool open = ImGui::TreeNodeEx("Assets", rootFlags);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::IsItemClicked()) m_CurrentDirectory = root;
				
				if (open) {
					DrawDirectoryTree(root);
					ImGui::TreePop();
				}
			}

			ImGui::TableSetColumnIndex(1);
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(m_CurrentDirectory)) {
				DrawDirectoryContent(m_CurrentDirectory);
			}

			ImGui::Separator();

			ImGui::SetNextItemWidth(100);
			ImGui::SliderFloat("Zoom", &m_ThumbnailSize, 32.0f, 128.0f);
			
			ImGui::EndTable();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_ShowDeleteModal) {
			ImGui::OpenPopup("Delete Asset?");
			m_ShowDeleteModal = false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopupModal("Delete Asset?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("Are you sure you want to delete '%s'?", m_AssetToDelete.filename().string().c_str());
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "This action cannot be undone!");
			ImGui::Separator();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Delete", ImVec2(120, 0))) {
				AssetDatabase::Instance().DeleteAsset(m_AssetToDelete);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SetItemDefaultFocus();
			ImGui::SameLine();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		DrawCreateAssetModal();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_ShowCreateScriptModal) {
			ImGui::OpenPopup("Create C# Script");
			m_ShowCreateScriptModal = false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopupModal("Create C# Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("New scripts are created in the currently selected asset folder.");
			ImGui::InputText("Class Name", m_NewScriptName.data(), m_NewScriptName.size());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!m_CreateScriptError.empty()) {
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_CreateScriptError.c_str());
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Create", ImVec2(120, 0))) {
				std::filesystem::path createdPath;
				std::string error;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ScriptCompilationSubsystem::Instance().CreateScriptAsset(m_CurrentDirectory, m_NewScriptName.data(), createdPath, &error)) {
					m_Owner->SetSelectedAsset(std::filesystem::absolute(createdPath));
					ImGui::CloseCurrentPopup();
				} else {
					m_CreateScriptError = error;
				}
			}
			ImGui::SameLine();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
}

void ProjectBrowserWindow::OpenCreateAssetModal(const std::string& title, const std::string& defaultName, const std::string& extension)
{
	m_CreateAssetTitle = title;
	m_CreateAssetExtension = extension;
	m_CreateAssetError.clear();
	std::fill(m_NewAssetName.begin(), m_NewAssetName.end(), '\0');
	std::copy(defaultName.begin(), defaultName.end(), m_NewAssetName.begin());
	m_ShowCreateAssetModal = true;
}

void ProjectBrowserWindow::DrawCreateAssetModal()
{
	if (m_ShowCreateAssetModal) {
		ImGui::OpenPopup(m_CreateAssetTitle.c_str());
		m_ShowCreateAssetModal = false;
	}

	if (ImGui::BeginPopupModal(m_CreateAssetTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::InputText("Name", m_NewAssetName.data(), m_NewAssetName.size());
		if (!m_CreateAssetError.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_CreateAssetError.c_str());
		}

		if (ImGui::Button("Create", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
			CreateNamedAsset();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void ProjectBrowserWindow::CreateNamedAsset()
{
	std::string assetName = m_NewAssetName.data();
	if (assetName.empty()) {
		m_CreateAssetError = "Name cannot be empty.";
		return;
	}

	const std::filesystem::path targetPath = m_CreateAssetExtension.empty()
		? MakeUniqueAssetPath(m_CurrentDirectory, assetName, "")
		: MakeUniqueAssetPath(m_CurrentDirectory, assetName, m_CreateAssetExtension);

	if (m_CreateAssetExtension.empty()) {
		std::filesystem::create_directory(targetPath);
		m_CurrentDirectory = targetPath;
		ImGui::CloseCurrentPopup();
		return;
	}

	if (m_CreateAssetExtension == ".tmat") {
		MaterialHeader header{};
		std::memcpy(header.magic, "TMAT", 4);
		header.version = kMaterialCurrentVersion;
		header.headerSize = sizeof(MaterialHeader);
		header.flags = 0;
		header.shadingModel = (uint32_t)ShadingModel::Unlit;
		header.alphaMode = (uint32_t)AlphaMode::Opaque;
		header.shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
		header.baseColorFactor[0] = 1.0f;
		header.baseColorFactor[1] = 1.0f;
		header.baseColorFactor[2] = 1.0f;
		header.baseColorFactor[3] = 1.0f;
		header.metallicFactor = 0.0f;
		header.roughnessFactor = 0.5f;
		header.normalScale = 1.0f;
		header.occlusionStrength = 1.0f;
		header.textureCount = 0;
		header.floatParameterCount = 0;
		header.textureArrayOffset = sizeof(MaterialHeader);
		header.floatParameterOffset = sizeof(MaterialHeader);

		std::ofstream out(targetPath, std::ios::binary);
		out.write(reinterpret_cast<const char*>(&header), sizeof(MaterialHeader));
	} else if (m_CreateAssetExtension == ".tuiml") {
		std::ofstream markupOutput(targetPath);
		markupOutput << kDefaultUIMarkup;
	} else if (m_CreateAssetExtension == ".teprefab") {
		nlohmann::json prefabJson;
		prefabJson["name"] = targetPath.stem().string();
		prefabJson["objects"] = nlohmann::json::array();
		std::ofstream prefabOutput(targetPath);
		prefabOutput << prefabJson.dump(4);
	}

	m_Owner->SetSelectedAsset(std::filesystem::absolute(targetPath));
	ImGui::CloseCurrentPopup();
}

// ProjectBrowserWindow::DrawDirectoryTree is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::DrawDirectoryTree(const std::filesystem::path& path) {
	if (!std::filesystem::exists(path)) return;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::directory_iterator(path)) {
		if (entry.path().extension() == ".meta") continue;
		if (!entry.is_directory()) continue;

		std::string filename = entry.path().filename().string();
		
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (entry.path() == m_CurrentDirectory) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		bool open = ImGui::TreeNodeEx(filename.c_str(), flags);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemClicked()) {
			m_CurrentDirectory = entry.path();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (open) {
			DrawDirectoryTree(entry.path());
			ImGui::TreePop();
		}
	}
}

// ProjectBrowserWindow::DrawDirectoryContent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::DrawDirectoryContent(const std::filesystem::path& path) {
	float cellSize = m_ThumbnailSize + m_Padding;
	float panelWidth = ImGui::GetContentRegionAvail().x;
	int columnCount = (int)(panelWidth / cellSize);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (columnCount < 1) columnCount = 1;

	std::vector<std::filesystem::directory_entry> entries;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::directory_iterator(path)) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (entry.path().extension() == ".meta") continue;
		entries.push_back(entry);
	}

	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (a.is_directory() != b.is_directory()) {
			return a.is_directory() > b.is_directory(); // Directories first
		}
		return a.path().filename().string() < b.path().filename().string();
	});

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("ContentTable", columnCount)) {
		for (const auto& entry : entries) {
			std::string filename = entry.path().filename().string();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!m_SearchFilter.empty()) {
				std::string lowerFilename = filename;
				std::string lowerFilter = m_SearchFilter;
				std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
				std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (lowerFilename.find(lowerFilter) == std::string::npos) continue;
			}

			ImGui::TableNextColumn();
			
			ImGui::PushID(filename.c_str());
			const std::filesystem::path absoluteEntryPath = std::filesystem::absolute(entry.path());
			
			std::string icon = entry.is_directory() ? "[DIR]" : "[FILE]";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!entry.is_directory() && entry.path().extension() == ".cs") {
				icon = "[C#]";
			} else if (!entry.is_directory() && entry.path().extension() == ".tuiml") {
				icon = "[UI]";
			} else if (!entry.is_directory() && entry.path().extension() == ".teprefab") {
				icon = "[PFB]";
			}
			
			bool isSelected = false;
			auto currentSelected = m_Owner->GetSelectedAsset();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentSelected && *currentSelected == absoluteEntryPath) {
				isSelected = true;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isSelected) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
			}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Button(icon.c_str(), ImVec2(m_ThumbnailSize, m_ThumbnailSize))) {
					if (!entry.is_directory()) {
						m_Owner->SetSelectedAsset(absoluteEntryPath);
					}
				}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isSelected) {
				ImGui::PopStyleColor();
			}

			// Bind the context menu to the asset icon. A window-level popup
			// inside this loop can reuse the wrong loop entry and delete an
			// unrelated asset when the browser background is right-clicked.
			if (ImGui::BeginPopupContextItem("AssetIconContextMenu")) {
				if (ImGui::MenuItem("Delete")) {
					m_AssetToDelete = absoluteEntryPath;
					m_ShowDeleteModal = true;
				}
				ImGui::EndPopup();
			}
			
			// Double-click performs the "open" action while single-click stays a
			// pure selection gesture, which matches the Unity-style asset flow.
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (entry.is_directory()) {
					m_CurrentDirectory = entry.path();
				} else {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (entry.path().extension() == ".tescene") {
						auto projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
						auto assetRoot = projectRoot / "Assets";
						std::error_code ec;
						auto relPath = std::filesystem::relative(entry.path(), assetRoot, ec);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (!ec) {
							m_Owner->RequestOpenScene(relPath);
						} else {
							m_Owner->RequestOpenScene(entry.path());
						}
					} else if (entry.path().extension() == ".cs") {
						ScriptCompilationSubsystem::Instance().OpenScript(std::filesystem::absolute(entry.path()));
					} else if (entry.path().extension() == ".tuiml") {
						OpenMarkupAssetInBuilder(m_Owner, entry.path());
					} else if (entry.path().extension() == ".teprefab") {
						m_Owner->OpenPrefabAsset(std::filesystem::absolute(entry.path()));
					}
				}
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!entry.is_directory() && ImGui::BeginDragDropSource()) {
				auto& assets = AssetDatabase::Instance().GetAssets();
				auto it = assets.find(std::filesystem::absolute(entry.path()));
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (it != assets.end()) {
					TerraEngine::UUID guid = it->second.guid;
					const std::string& importer = it->second.importerName;
					const char* payloadType = "ASSET_GUID";
					
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (importer == "MeshImporter") payloadType = "ASSET_MESH";
					else if (importer == "TextureImporter") payloadType = "ASSET_TEXTURE";
					else if (importer == "ShaderImporter") payloadType = "ASSET_SHADER";
					else if (importer == "MaterialImporter") payloadType = "ASSET_MATERIAL";
					else if (importer == "ScriptImporter") payloadType = "ASSET_SCRIPT";
					else if (importer == "UIImporter") payloadType = "ASSET_TUIML";
					else if (importer == "StyleImporter") payloadType = "ASSET_TUSS";

					ImGui::SetDragDropPayload(payloadType, &guid, sizeof(TerraEngine::UUID));
					ImGui::Text("%s", filename.c_str());
				}
				ImGui::EndDragDropSource();
			}

			ImGui::TextWrapped("%s", filename.c_str());
			if (ImGui::BeginPopupContextItem("AssetNameContextMenu")) {
				if (ImGui::MenuItem("Delete")) {
					m_AssetToDelete = absoluteEntryPath;
					m_ShowDeleteModal = true;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

// ProjectBrowserWindow::Reset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectBrowserWindow::Reset() {
	m_CurrentDirectory.clear();
}
