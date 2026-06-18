#include "TestHarness.h"

#include "archive/ArchiveWorkflow.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		tepak::PackagingRequest MakeValidRequest() {
			tepak::PackagingRequest request;
			request.inputRoot = std::filesystem::temp_directory_path() / "TerraEngine_packaging_request_input";
			request.outputPackPath = std::filesystem::temp_directory_path() / "TerraEngine_packaging_request_output.tepak";
			request.workerCount = 1U;
			request.extentSizingBytes = 4096U;
			request.peakMemoryBudgetBytes = 1024U * 1024U;
			return request;
		}

		bool ThrowsMessageContaining(const std::function<void()>& operation, const std::string& expectedText) {
			try {
				operation();
			} catch(const std::runtime_error& error) {
				return std::string(error.what()).find(expectedText) != std::string::npos;
			}
			return false;
		}
	}

	void RegisterPackagingRequestValidationTests(TestRegistry& registry) {
		registry.Add("Editor.PackagingRequestValidation.RequiresInputRoot", [](TestContext& context) {
			tepak::PackagingRequest request = MakeValidRequest();
			request.inputRoot.clear();

			context.Expect(ThrowsMessageContaining([&request]() {
				(void)tepak::archive::BuildPackagingInventory(request);
			}, "input root"), "BuildPackagingInventory should reject requests without an input root before filesystem discovery.");
		});

		registry.Add("Editor.PackagingRequestValidation.RequiresOutputPackPath", [](TestContext& context) {
			tepak::PackagingRequest request = MakeValidRequest();
			request.outputPackPath.clear();

			context.Expect(ThrowsMessageContaining([&request]() {
				(void)tepak::archive::BuildPackagingInventory(request);
			}, "output pack path"), "BuildPackagingInventory should reject requests without an output pack path before filesystem discovery.");
		});

		registry.Add("Editor.PackagingRequestValidation.RequiresAtLeastOneWorker", [](TestContext& context) {
			tepak::PackagingRequest request = MakeValidRequest();
			request.workerCount = 0U;

			context.Expect(ThrowsMessageContaining([&request]() {
				(void)tepak::archive::BuildPackagingInventory(request);
			}, "worker"), "BuildPackagingInventory should reject requests with no workers.");
		});

		registry.Add("Editor.PackagingRequestValidation.RequiresMinimumExtentSize", [](TestContext& context) {
			tepak::PackagingRequest request = MakeValidRequest();
			request.extentSizingBytes = 4095U;

			context.Expect(ThrowsMessageContaining([&request]() {
				(void)tepak::archive::BuildPackagingInventory(request);
			}, "4096"), "BuildPackagingInventory should reject extent sizes smaller than the archive minimum.");
		});

		registry.Add("Editor.PackagingRequestValidation.RequiresPositiveMemoryBudget", [](TestContext& context) {
			tepak::PackagingRequest request = MakeValidRequest();
			request.peakMemoryBudgetBytes = 0U;

			context.Expect(ThrowsMessageContaining([&request]() {
				(void)tepak::archive::BuildPackagingInventory(request);
			}, "memory budget"), "BuildPackagingInventory should reject requests without a positive memory budget.");
		});
	}
}
