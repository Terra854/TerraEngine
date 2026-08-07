#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadHierarchyWindowSource()
		{
			const auto read = [](const std::filesystem::path& path) {
				std::ifstream input(path, std::ios::binary);
				if (!input)
				{
					return std::string();
				}
				std::ostringstream contents;
				contents << input.rdbuf();
				return contents.str();
			};

			return read(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/HierarchyWindow.cpp");
		}
	}

	void RegisterHierarchyVirtualizationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Hierarchy.Virtualization.ExpandedRowsUseStableClippedCache", [](TestContext& context) {
			const std::string source = ReadHierarchyWindowSource();
			context.Expect(!source.empty(), "HierarchyWindow source should be available to the editor contract test.");
			context.Expect(source.find("m_CachedVisibleRows") != std::string::npos,
				"Hierarchy rendering should retain a stable visible-row cache.");
			context.Expect(source.find("m_ExpandedByUuid") != std::string::npos,
				"Hierarchy expansion state should be keyed by authored UUID.");
			context.Expect(source.find("ImGuiListClipper") != std::string::npos,
				"Hierarchy rows should be submitted through ImGuiListClipper.");
			context.Expect(source.find("DrawVisibleRow(m_CachedVisibleRows") != std::string::npos,
				"The clipper should draw rows from the complete visible hierarchy list.");
			context.Expect(source.find("m_CachedExpansionRevision == m_ExpansionRevision") != std::string::npos,
				"Expansion changes should invalidate the visible-row cache.");
			context.Expect(source.find("m_CachedVisibleHierarchyRevision == hierarchyRevision") != std::string::npos,
				"The visible-row cache needs its own hierarchy revision because root-cache refresh updates a separate revision first.");
			context.Expect(source.find("m_CachedVisibleSceneGeneration == sceneGeneration") != std::string::npos
				&& source.find("sceneGeneration == m_CachedSceneGeneration") != std::string::npos,
				"Play Mode scene swaps must invalidate both hierarchy caches even if handles or hierarchy revision values are reused.");
			context.Expect(source.find("ImGui::GetFrameHeightWithSpacing()") != std::string::npos,
				"The list clipper should use a fixed row height so a boundary-stale row cannot assert during height inference.");
			context.Expect(source.find("ImGui::Indent(indentation)") != std::string::npos
				&& source.find("ImGui::Unindent(indentation)") != std::string::npos,
				"Flattened descendants should retain their visual depth without keeping clipped parents on the ImGui stack.");
			context.Expect(source.find("if (opened)") != std::string::npos
				&& source.find("if (drawChildren)") != std::string::npos
				&& source.find("ImGui::TreePop()") != std::string::npos,
				"Virtualized open rows must still balance ImGui tree pushes with TreePop.");
		});
	}
}
