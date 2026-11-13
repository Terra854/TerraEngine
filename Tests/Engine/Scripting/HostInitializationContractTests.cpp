// Exercise repeated initialization through a real dynamically loaded ABI fixture.
#include "TestHarness.h"
#include "DotNet/DotNetHost.h"
#include "Scripting/ExportVTable.h"
#include "Utilities/PlatformUtils.h"

#include <filesystem>

#ifndef TERRAENGINE_DOTNET_HOST_FIXTURE_PATH
#define TERRAENGINE_DOTNET_HOST_FIXTURE_PATH ""
#endif

namespace TerraEngine::Tests {
	namespace {
		struct IsolatedExportHost {
			DotNetHost& host = DotNetHost::Instance();
			IsolatedExportHost() { host.Shutdown(); }
			~IsolatedExportHost() { host.Shutdown(); }
		};
		std::filesystem::path FixturePath(TestContext& context) {
			// Only the configured filename is portable. The fixture is installed
			// beside this executable, so never load an old library from the build tree.
			const auto filename = std::filesystem::path(TERRAENGINE_DOTNET_HOST_FIXTURE_PATH).filename();
			const auto executable = PlatformUtils::GetExecutablePath();
			context.Expect(!filename.empty() && !executable.empty(),
				"Configure the DotNetHost fixture filename and provide executable-path discovery.");
			const auto path = executable.parent_path() / filename;
			std::error_code error;
			context.Expect(std::filesystem::is_regular_file(path, error) && !error
				&& std::filesystem::file_size(path, error) > 0U && !error,
				"Build or install the nonempty DotNetHost ABI fixture beside the test executable.");
			return path;
		}
	}
	void RegisterHostInitializationContractTests(TestRegistry& registry) {
		registry.Add("Scripting.HostInitialization.EverySuccessfulCallFillsFreshTable", [](TestContext& context) {
			IsolatedExportHost fixture;
			const auto path = FixturePath(context);
			ManagedVTable first{}, second{};
			context.Expect(fixture.host.InitializeExportRuntime(path, &first), "First initialization must load the real ABI fixture.");
			context.Expect(first.getScriptCount != nullptr && first.getScriptCount() == 17,
				"First initialization must expose a callable fixture function.");
			context.Expect(fixture.host.InitializeExportRuntime(path, &second), "Repeated initialization must succeed with fresh caller storage.");
			context.Expect(second.version == first.version && second.getScriptCount == first.getScriptCount
				&& second.getScriptIds == first.getScriptIds && second.createScript == first.createScript
				&& second.invokeUpdate == first.invokeUpdate && second.getScriptCount != nullptr,
				"Every successful initialization must populate the fresh caller's released table prefix.");
			ManagedVTable rejected{};
			context.Expect(!fixture.host.InitializeExportRuntime(path.parent_path() / "missing-runtime.dll", &rejected) && rejected.getScriptCount == nullptr, "A missing requested library cannot reuse a cached table.");
			// Canonicalization must not erase the actionable missing-runtime message.
			context.Expect(fixture.host.GetLastNativeError().find("missing-runtime.dll") != std::string::npos
				&& fixture.host.GetLastNativeError().find("Managed/") != std::string::npos,
				"A missing library must retain its requested path and deployment guidance.");
			// The executable is a real existing file with a different physical identity.
			context.Expect(!fixture.host.InitializeExportRuntime(PlatformUtils::GetExecutablePath(), &rejected) && rejected.getScriptCount == nullptr, "A different existing library path must reject the cached fast path.");
			context.Expect(second.getScriptCount() == 17, "Repeated initialization must retain callable pointers while the library is loaded.");
		});
		registry.Add("Scripting.HostInitialization.InitializedHostRejectsNullOutput", [](TestContext& context) {
			IsolatedExportHost fixture;
			const auto path = FixturePath(context);
			ManagedVTable table{};
			context.Expect(fixture.host.InitializeExportRuntime(path, &table), "The fixture must initialize before testing the fast path.");
			context.Expect(!fixture.host.InitializeExportRuntime(path, nullptr), "An initialized host cannot report success without a destination table.");
		});
		registry.Add("Scripting.HostInitialization.ShutdownAllowsFreshLoadedTable", [](TestContext& context) {
			IsolatedExportHost fixture;
			const auto path = FixturePath(context);
			ManagedVTable first{};
			context.Expect(fixture.host.InitializeExportRuntime(path, &first), "The fixture must load initially.");
			fixture.host.Shutdown();
			context.Expect(!fixture.host.IsInitialized() && !fixture.host.IsExportMode(), "Shutdown must reset both lifecycle flags.");
			ManagedVTable fresh{};
			context.Expect(fixture.host.InitializeExportRuntime(path, &fresh) && fresh.getScriptCount != nullptr
				&& fresh.getScriptCount() == 17, "A fresh load must populate new storage after shutdown.");
		});
	}
}
