#include "ProjectBrowserWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "Engine/Resource/ResourceManager.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <vector>
#include <fstream>
#include "../../../Engine/File Formats/TerraEngineMaterialFile.h"

ProjectBrowserWindow::ProjectBrowserWindow(MainEditorSubsystem* owner)
    : EditorWindow("Project Browser"), m_Owner(owner) {
}

void ProjectBrowserWindow::OnGui() {
    if (ImGuiFileDialog::Instance()->Display("ImportAssetDlg")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::filesystem::path filePath = ImGuiFileDialog::Instance()->GetFilePathName();
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

    if (m_Owner->GetContext() && m_Owner->GetContext()->projectManager->HasActiveProject()) {
        std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
        if (m_CurrentDirectory.empty() || !std::filesystem::exists(m_CurrentDirectory)) {
            m_CurrentDirectory = root;
        }

        if (ImGui::Button("Add")) {
            ImGui::OpenPopup("AddAssetPopup");
        }

        if (ImGui::BeginPopup("AddAssetPopup")) {
            if (ImGui::MenuItem("Folder")) {
                std::filesystem::path newFolderPath = m_CurrentDirectory / "New Folder";
                int i = 1;
                while (std::filesystem::exists(newFolderPath)) {
                    newFolderPath = m_CurrentDirectory / ("New Folder (" + std::to_string(i++) + ")");
                }
                std::filesystem::create_directory(newFolderPath);
            }
            if (ImGui::MenuItem("Material (.tmat)")) {
                std::filesystem::path newMatPath = m_CurrentDirectory / "New Material.tmat";
                int i = 1;
                while (std::filesystem::exists(newMatPath)) {
                    newMatPath = m_CurrentDirectory / ("New Material (" + std::to_string(i++) + ").tmat");
                }

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

                std::ofstream out(newMatPath, std::ios::binary);
                out.write((char*)&header, sizeof(MaterialHeader));
                
                m_Owner->SetSelectedAsset(std::filesystem::absolute(newMatPath));
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        char searchBuf[128];
        strncpy(searchBuf, m_SearchFilter.c_str(), sizeof(searchBuf));
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputTextWithHint("##Search", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf))) {
            m_SearchFilter = searchBuf;
        }

        ImGui::Separator();

        if (ImGui::BeginTable("BrowserSplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
             ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthFixed, 200.0f);
             ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
             
             ImGui::TableNextRow();
             ImGui::TableSetColumnIndex(0);
             
             if (std::filesystem::exists(root)) {
                 ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                 if (root == m_CurrentDirectory) rootFlags |= ImGuiTreeNodeFlags_Selected;
                 
                 bool open = ImGui::TreeNodeEx("Assets", rootFlags);
                 if (ImGui::IsItemClicked()) m_CurrentDirectory = root;
                 
                 if (open) {
                     DrawDirectoryTree(root);
                     ImGui::TreePop();
                 }
             }

             ImGui::TableSetColumnIndex(1);
             
             if (std::filesystem::exists(m_CurrentDirectory)) {
                 DrawDirectoryContent(m_CurrentDirectory);
             }

             ImGui::Separator();

             ImGui::SetNextItemWidth(100);
             ImGui::SliderFloat("Zoom", &m_ThumbnailSize, 32.0f, 128.0f);
             
             ImGui::EndTable();
        }
    }
}

void ProjectBrowserWindow::DrawDirectoryTree(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return;

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".meta") continue;
        if (!entry.is_directory()) continue;

        std::string filename = entry.path().filename().string();
        
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entry.path() == m_CurrentDirectory) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        bool open = ImGui::TreeNodeEx(filename.c_str(), flags);
        if (ImGui::IsItemClicked()) {
            m_CurrentDirectory = entry.path();
        }

        if (open) {
            DrawDirectoryTree(entry.path());
            ImGui::TreePop();
        }
    }
}

void ProjectBrowserWindow::DrawDirectoryContent(const std::filesystem::path& path) {
    float cellSize = m_ThumbnailSize + m_Padding;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = (int)(panelWidth / cellSize);
    if (columnCount < 1) columnCount = 1;

    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".meta") continue;
        entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.is_directory() != b.is_directory()) {
            return a.is_directory() > b.is_directory(); // Directories first
        }
        return a.path().filename().string() < b.path().filename().string();
    });

    if (ImGui::BeginTable("ContentTable", columnCount)) {
        for (const auto& entry : entries) {
            std::string filename = entry.path().filename().string();
            if (!m_SearchFilter.empty()) {
                std::string lowerFilename = filename;
                std::string lowerFilter = m_SearchFilter;
                std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
                std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
                if (lowerFilename.find(lowerFilter) == std::string::npos) continue;
            }

            ImGui::TableNextColumn();
            
            ImGui::PushID(filename.c_str());
            
            std::string icon = entry.is_directory() ? "[DIR]" : "[FILE]";
            
            bool isSelected = false;
            auto currentSelected = m_Owner->GetSelectedAsset();
            if (currentSelected && *currentSelected == std::filesystem::absolute(entry.path())) {
                isSelected = true;
            }

            if (isSelected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
            }

            if (ImGui::Button(icon.c_str(), ImVec2(m_ThumbnailSize, m_ThumbnailSize))) {
                if (!entry.is_directory()) {
                    m_Owner->SetSelectedAsset(std::filesystem::absolute(entry.path()));
                }
            }

            if (isSelected) {
                ImGui::PopStyleColor();
            }
            
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.is_directory()) {
                    m_CurrentDirectory = entry.path();
                }
            }
            
             if (!entry.is_directory() && ImGui::BeginDragDropSource()) {
                auto& assets = AssetDatabase::Instance().GetAssets();
                auto it = assets.find(std::filesystem::absolute(entry.path()));
                if (it != assets.end()) {
                    TerraEngine::UUID guid = it->second.guid;
                    const std::string& importer = it->second.importerName;
                    const char* payloadType = "ASSET_GUID";
                    
                    if (importer == "MeshImporter") payloadType = "ASSET_MESH";
                    else if (importer == "TextureImporter") payloadType = "ASSET_TEXTURE";
                    else if (importer == "ShaderImporter") payloadType = "ASSET_SHADER";
                    else if (importer == "MaterialImporter") payloadType = "ASSET_MATERIAL";

                    ImGui::SetDragDropPayload(payloadType, &guid, sizeof(TerraEngine::UUID));
                    ImGui::Text("%s", filename.c_str());
                }
                ImGui::EndDragDropSource();
            }

            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
