#include "TestHarness.h"

#include "Runtime/ContentMount/ContentMount.h"

#include <filesystem>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	void RegisterContentMountPublicApiTests(TestRegistry& registry) {
		registry.Add("Engine.ContentMount.NormalizeLogicalPathAppliesRuntimeLookupPolicy", [](TestContext& context) {
			context.ExpectEqual(std::string("textures/hero.png"), ContentMount::NormalizeLogicalPath("./Textures\\Hero.PNG"), "Relative logical paths should be lowercased and converted to forward slashes.");
			context.ExpectEqual(std::string("root/folder/file.tescene"), ContentMount::NormalizeLogicalPath("\\Root\\Folder\\File.TESCENE"), "Leading slashes should be stripped from logical paths.");
			context.ExpectEqual(std::string("already/normalized.asset"), ContentMount::NormalizeLogicalPath("already/normalized.asset"), "Already-normalized logical paths should remain stable.");
		});

		registry.Add("Engine.ContentMount.UnmountedReadsFailClosed", [](TestContext& context) {
			ContentMount mount;

			context.Expect(!mount.IsMounted(), "A newly constructed content mount should start unmounted.");
			context.Expect(mount.GetArchivePath().empty(), "A newly constructed content mount should not report an archive path.");
			context.Expect(!mount.Exists("game.json"), "Exists should fail closed when no archive is mounted.");
			context.Expect(mount.ReadAll("game.json").empty(), "ReadAll should return an empty buffer when no archive is mounted.");
			context.Expect(mount.ReadRange("game.json", 0U, 16U).empty(), "ReadRange should return an empty buffer when no archive is mounted.");
		});

		registry.Add("Engine.ContentMount.FailedMountLeavesStateUnmounted", [](TestContext& context) {
			ContentMount mount;
			const std::filesystem::path missingArchive = std::filesystem::temp_directory_path() / "TerraEngine_missing_public_api_test.tepak";
			std::filesystem::remove(missingArchive);

			const bool mounted = mount.Mount(missingArchive);
			mount.Unmount();
			mount.Unmount();

			context.Expect(!mounted, "Mount should return false when the archive path does not exist.");
			context.Expect(!mount.IsMounted(), "A failed mount attempt should leave the mount unmounted.");
			context.Expect(mount.GetArchivePath().empty(), "A failed mount attempt should not publish an archive path.");
			context.Expect(!mount.Exists("game.json"), "Exists should remain false after a failed mount attempt.");
		});
	}
}
