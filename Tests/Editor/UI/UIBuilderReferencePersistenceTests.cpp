#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Windows/UIBuilderWindow.h"
#include "Resource/ResourceManager.h"
#include "UI/UIParser.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace TerraEngine::EditorTests {
	namespace {
		struct BuilderReferenceDirectory final {
			std::filesystem::path root;
			std::filesystem::path path;
			BuilderReferenceDirectory() {
				static std::atomic<unsigned long long> serial{0U};
				root = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
				if (root.filename().empty()) root = root.parent_path();
				for (unsigned attempt = 0; attempt < 4096U; ++attempt) {
					const auto candidate = root / ("TerraEngine_BuilderReferences-" + std::to_string(serial.fetch_add(1U)));
					std::error_code error;
					if (std::filesystem::create_directory(candidate, error)) { path = candidate; return; }
					if (error && error != std::errc::file_exists) throw std::filesystem::filesystem_error("Reserve builder fixture", candidate, error);
				}
				throw std::runtime_error("Builder fixture directory capacity exceeded.");
			}
			~BuilderReferenceDirectory() {
				// Remove only the directory exclusively reserved by this instance.
				if (!path.empty() && path.is_absolute() && path.parent_path() == root) {
					std::error_code ignored;
					std::filesystem::remove_all(path, ignored);
				}
			}
			BuilderReferenceDirectory(const BuilderReferenceDirectory&) = delete;
			BuilderReferenceDirectory& operator=(const BuilderReferenceDirectory&) = delete;
		};
		std::string ReadBuilderBytes(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}
		struct RestoreBuilderAssetRoot final {
			std::filesystem::path previous = ResourceManager::Instance().GetAssetRoot();
			~RestoreBuilderAssetRoot() { ResourceManager::Instance().SetAssetRoot(previous); }
		};
	}
	void RegisterUIBuilderReferencePersistenceTests(TestRegistry& registry) {
		registry.Add("Editor.UIBuilder.DetachSavePreservesSharedStylesheetAsset", [](TestContext& context) {
			const BuilderReferenceDirectory directory;
			RestoreBuilderAssetRoot restore;
			ResourceManager::Instance().SetAssetRoot(directory.path);
			const auto first = directory.path / "first.tuiml";
			const auto second = directory.path / "second.tuiml";
			// Use the legacy sibling name so reopening would expose accidental inference.
			const auto style = directory.path / "first.tuss";
			{ std::ofstream output(style); output << ".shared { color: #abcdef; }\n"; }
			for (const auto& markup : {first, second}) {
				std::ofstream output(markup);
				output << "<UIRoot styleSrc=\"first.tuss\"><Label class=\"shared\" text=\"Shared\" /></UIRoot>";
			}
			const auto originalStyle = ReadBuilderBytes(style);
			const auto originalOtherDocument = ReadBuilderBytes(second);
			UIBuilderWindow window(nullptr);
			window.OpenMarkupDocument(first);
			context.ExpectEqual(std::size_t{1}, UIBuilderWindowTestAccess::StyleCount(window), "Actual open path must attach the shared stylesheet.");
			// These accessors invoke the actual editor button operations, preserving
			// the real session state and persistence path instead of simulating detach.
			UIBuilderWindowTestAccess::Detach(window, 0);
			context.Expect(UIBuilderWindowTestAccess::Save(window), "The real builder save must succeed after detach.");
			context.Expect(ReadBuilderBytes(style) == originalStyle, "Detaching a document reference must preserve the shared authored asset byte-for-byte.");
			context.Expect(ReadBuilderBytes(second) == originalOtherDocument, "The other referencing document must remain unchanged.");
			const UIParser parser;
			const auto reopened = parser.ParseMarkupString(ReadBuilderBytes(first), "DetachedDocument");
			window.OpenMarkupDocument(first);
			context.ExpectEqual(std::size_t{0}, UIBuilderWindowTestAccess::StyleCount(window), "Actual reopen must honor an explicit empty style list despite the preserved sibling asset.");
			context.Expect(reopened && reopened->hasExplicitStyleSheetSources && reopened->styleSheetSources.empty(), "Reopening the saved document must show the detached reference.");
			const auto other = parser.ParseMarkupString(ReadBuilderBytes(second), "OtherDocument");
			context.Expect(other && other->styleSheetSources == std::vector<std::string>{"first.tuss"}, "The other document must retain its authored shared reference.");
		});
	}
}
