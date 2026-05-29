#include "TestHarness.h"

#include "ExportValidator.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		class TempManagedSourceDirectory {
		public:
			TempManagedSourceDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "ManagedSourceValidation") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempManagedSourceDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

			void WriteSource(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = root_ / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath);
				stream << contents;
			}

		private:
			std::filesystem::path root_;
		};

		bool HasManagedDiagnosticContaining(const ExportValidationResult& result, const std::string& text) {
			return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&text](const ExportDiagnostic& diagnostic) {
				return diagnostic.category == "ManagedSource" && diagnostic.message.find(text) != std::string::npos;
			});
		}
	}

	void RegisterExportValidatorManagedSourceTests(TestRegistry& registry) {
		registry.Add("Editor.ExportValidator.ManagedSourceIgnoresCommentsAndStringLiterals", [](TestContext& context) {
			TempManagedSourceDirectory tempDirectory;
			tempDirectory.WriteSource("Scripts/Safe.cs",
R"(public sealed class SafeScript {
	private string label = "dynamic Type.GetType Activator.CreateInstance";
	// dynamic commentedValue = null;
	/* System.Reflection.Emit and ILGenerator in a block comment should not count. */
	private int @dynamic = 1;
	[SerializeField] private int Health;
})");

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateManagedSource(tempDirectory.Root());

			context.Expect(result.passed, "Managed source validation should ignore unsupported tokens inside comments and string literals.");
			context.ExpectEqual(0u, result.errorCount, "Comment and string literal mentions should not create managed-source errors.");
			context.Expect(HasManagedDiagnosticContaining(result, "Scanned 1 C# source file"), "The validator should report the number of scanned source files.");
		});

		registry.Add("Editor.ExportValidator.ManagedSourceReportsUnsupportedNativeAotPatterns", [](TestContext& context) {
			TempManagedSourceDirectory tempDirectory;
			tempDirectory.WriteSource("Scripts/Unsafe.cs",
R"(using System.Reflection.Emit;

public sealed class UnsafeScript {
	public void Run() {
		dynamic value = 1;
		var type = Type.GetType("Game.UnsafeScript");
		var created = Activator.CreateInstance(type);
	}

	[SerializeField] private System.Collections.Generic.List<int> Values;
	[SerializeField] private string Name;
})");

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateManagedSource(tempDirectory.Root());

			context.Expect(!result.passed, "Unsupported NativeAOT managed-source patterns should fail validation.");
			context.Expect(result.errorCount >= 5u, "Reflection, dynamic, and unsupported serialized fields should each produce errors.");
			context.Expect(HasManagedDiagnosticContaining(result, "Reflection API usage detected"), "Reflection API diagnostics should be reported.");
			context.Expect(HasManagedDiagnosticContaining(result, "'dynamic' keyword usage detected"), "Dynamic keyword diagnostics should be reported.");
			context.Expect(HasManagedDiagnosticContaining(result, "Unsupported [SerializeField] type"), "Unsupported serialized field diagnostics should be reported.");
		});

		registry.Add("Editor.ExportValidator.ManagedSourceSkipsGeneratedProjectFolders", [](TestContext& context) {
			TempManagedSourceDirectory tempDirectory;
			tempDirectory.WriteSource("Build/Generated.cs", "public class Generated { dynamic Value; }");
			tempDirectory.WriteSource("Scripts/Safe.cs", "public class Safe { [SerializeField] private int Health; }");

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateManagedSource(tempDirectory.Root());

			context.Expect(result.passed, "Managed-source validation should skip generated project folders such as Build.");
			context.ExpectEqual(0u, result.errorCount, "Skipped generated folders should not contribute errors.");
			context.Expect(HasManagedDiagnosticContaining(result, "Scanned 1 C# source file"), "Only the authored safe source file should be counted as scanned.");
		});
	}
}
