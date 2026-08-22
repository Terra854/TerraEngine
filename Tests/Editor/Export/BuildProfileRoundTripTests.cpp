#include "TestHarness.h"

#include "BuildProfile.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		std::filesystem::path MakeTempProfilePath() {
			return std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "BuildProfiles" / "round-trip-profile.json";
		}

		NativeAotPublishContext MakeNativeAotTestContext() {
			// Command tests use deliberately distinct paths so an omitted option
			// cannot accidentally pass by matching another publish argument.
			return {
				"C:/Installed Editor/DotNet/dotnet.exe",
				"C:/Installed Editor/NativeAotNuGet",
				"C:/Installed Editor/NativeAotNuGet/NuGet.Config",
				"C:/Installed Editor/NativeAotNuGet/feed",
				"C:/Temp/TEAOT-command-test/packages",
				"C:/Temp/TEAOT-command-test/dotnet-home",
				"C:/Temp/TEAOT-command-test/obj",
				"C:/Temp/TEAOT-command-test/bin"
			};
		}

		class TempProfileDirectory {
		public:
			TempProfileDirectory()
				: root_(MakeTempProfilePath().parent_path().parent_path()) {
				std::filesystem::remove_all(root_);
			}

			~TempProfileDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

		private:
			std::filesystem::path root_;
		};
	}

	void RegisterBuildProfileRoundTripTests(TestRegistry& registry) {
		registry.Add("Editor.BuildProfile.SaveLoadRoundTripPreservesFields", [](TestContext& context) {
			TempProfileDirectory tempDirectory;
			const std::filesystem::path profilePath = MakeTempProfilePath();

			BuildProfile profile{};
			profile.name = "Automated Test Profile";
			profile.id = "testprofile01";
			profile.platform = ExportPlatform::Linux;
			profile.outputDirectory = "Builds/AutomatedTest";
			profile.startupScene = "Scenes/TestScene.tescene";
			profile.compressionPreset = CompressionPreset::Best;
			profile.tepakWorkerCount = 4;
			profile.enableManagedCompilation = false;
			profile.includeDebugSymbols = true;
			profile.stripNativeAotOutput = true;
			profile.enableVulkanValidationLayers = true;
			profile.enableRenderDebugDiagnostics = true;
			profile.additionalPublishArgs = "--self-contained true";
			profile.virtualGeometryQualityLevel = 3u;
			profile.virtualGeometryDiagnosticsEnabled = true;
			profile.virtualGeometryBudgetBytes = 256ull * 1024ull * 1024ull;
			profile.virtualGeometryLodErrorScale = 1.75f;
			profile.virtualGeometryMaxLodDrop = 3u;
			profile.virtualGeometryHiZOcclusionEnabled = true;
			profile.virtualGeometryPreloadBias = 2.0f;
			profile.includedScenes = {"Scenes/TestScene.tescene", "Scenes/Secondary.tescene"};

			context.Expect(SaveBuildProfile(profile, profilePath), "Saving a complete build profile should succeed.");

			const BuildProfile loaded = LoadBuildProfile(profilePath);
			context.ExpectEqual(profile.name, loaded.name, "The profile name should round-trip through JSON.");
			context.ExpectEqual(profile.id, loaded.id, "The profile id should round-trip through JSON.");
			context.Expect(loaded.platform == ExportPlatform::Linux, "The selected platform should round-trip through JSON.");
			context.ExpectEqual(profile.outputDirectory, loaded.outputDirectory, "The output directory should round-trip through JSON.");
			context.ExpectEqual(profile.startupScene, loaded.startupScene, "The startup scene should round-trip through JSON.");
			context.Expect(loaded.compressionPreset == CompressionPreset::Best, "The compression preset should round-trip through JSON.");
			context.ExpectEqual(profile.tepakWorkerCount, loaded.tepakWorkerCount, "The worker count should round-trip through JSON.");
			context.ExpectEqual(profile.enableManagedCompilation, loaded.enableManagedCompilation, "The managed compilation toggle should round-trip through JSON.");
			context.ExpectEqual(profile.includeDebugSymbols, loaded.includeDebugSymbols, "The debug symbol toggle should round-trip through JSON.");
			context.ExpectEqual(profile.stripNativeAotOutput, loaded.stripNativeAotOutput, "The NativeAOT strip toggle should round-trip through JSON.");
			context.ExpectEqual(profile.enableVulkanValidationLayers, loaded.enableVulkanValidationLayers, "The export Vulkan validation toggle should round-trip through JSON.");
			context.ExpectEqual(profile.enableRenderDebugDiagnostics, loaded.enableRenderDebugDiagnostics, "The export render debug diagnostics toggle should round-trip through JSON.");
			context.ExpectEqual(profile.additionalPublishArgs, loaded.additionalPublishArgs, "Additional publish arguments should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryQualityLevel, loaded.virtualGeometryQualityLevel, "Virtual geometry export quality should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryDiagnosticsEnabled, loaded.virtualGeometryDiagnosticsEnabled, "Virtual geometry diagnostics should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryBudgetBytes, loaded.virtualGeometryBudgetBytes, "Virtual geometry geometry budget should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryLodErrorScale, loaded.virtualGeometryLodErrorScale, "Virtual geometry LOD error scale should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryMaxLodDrop, loaded.virtualGeometryMaxLodDrop, "Virtual geometry max LOD drop should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryHiZOcclusionEnabled, loaded.virtualGeometryHiZOcclusionEnabled, "Virtual geometry Hi-Z occlusion toggle should round-trip through JSON.");
			context.ExpectEqual(profile.virtualGeometryPreloadBias, loaded.virtualGeometryPreloadBias, "Virtual geometry preload bias should round-trip through JSON.");
			context.ExpectEqual(profile.includedScenes.size(), loaded.includedScenes.size(), "All included scenes should round-trip through JSON.");
			context.ExpectEqual(profilePath.string(), loaded.filePath.string(), "The loaded profile should remember its source file path.");
		});

		registry.Add("Editor.BuildProfile.NativeAotPublishCommandHonorsStripToggle", [](TestContext& context) {
			BuildProfile profile{};
			profile.platform = ExportPlatform::Windows;
			profile.includeDebugSymbols = true;
			profile.stripNativeAotOutput = false;

			const std::filesystem::path projectPath = "C:/Projects/Terra Game/Assembly-CSharp.csproj";
			const std::filesystem::path outputPath = "C:/Projects/Terra Game/Builds/Profile/Managed";
			const NativeAotPublishContext toolchain = MakeNativeAotTestContext();

			const std::string unstrippedCommand = BuildNativeAotPublishCommand(profile, projectPath, outputPath, toolchain);
			context.Expect(unstrippedCommand.find("-p:PublishAot=true") != std::string::npos, "NativeAOT publish should always enable PublishAot.");
			context.Expect(unstrippedCommand.find("--no-restore") != std::string::npos, "NativeAOT publish should consume the explicit offline restore without performing a second restore.");
			context.Expect(unstrippedCommand.find("-p:StripSymbols=true") == std::string::npos, "Disabled strip output should not pass StripSymbols to dotnet.");
			context.Expect(unstrippedCommand.find("-p:DebugType=none") == std::string::npos, "Included debug symbols should not suppress debug output.");

			profile.stripNativeAotOutput = true;
			const std::string strippedCommand = BuildNativeAotPublishCommand(profile, projectPath, outputPath, toolchain);
			context.Expect(strippedCommand.find("-p:StripSymbols=true") != std::string::npos, "Enabled strip output should pass StripSymbols to dotnet.");
		});

		registry.Add("Editor.BuildProfile.NativeAotPublishCommandKeepsOtherPublishSettings", [](TestContext& context) {
			BuildProfile profile{};
			profile.platform = ExportPlatform::Linux;
			profile.includeDebugSymbols = false;
			profile.stripNativeAotOutput = true;
			profile.additionalPublishArgs =
				"-p:InvariantGlobalization=true "
				"-p:BaseIntermediateOutputPath=C:/Attacker/obj/ "
				"-p:MSBuildProjectExtensionsPath=C:/Attacker/extensions/ "
				"-p:BaseOutputPath=C:/Attacker/bin/ "
				"-p:OutputPath=C:/Attacker/output/ "
				"-p:PublishDir=C:/Attacker/publish/ "
				"-o C:/Attacker/cli-output/ "
				"-p:RestorePackagesPath=C:/Attacker/packages/ "
				"-p:DefaultItemExcludes=C:/Attacker/generated/** "
				"-p:RestoreSources=https://example.invalid/v3/index.json "
				"-p:RestoreFallbackFolders=C:/Attacker/fallback/ "
				"-p:RestoreAdditionalProjectSources=https://additional.invalid/ "
				"-p:NuGetAudit=true --no-restore";
			const NativeAotPublishContext toolchain = MakeNativeAotTestContext();
			const std::filesystem::path publishDirectory =
				"C:/Temp/TEAOT-command-test/publish";

			const std::string command = BuildNativeAotPublishCommand(
				profile,
				"/tmp/Terra Game/Assembly-CSharp.csproj",
				publishDirectory,
				toolchain);

			// These checks make sure the strip option does not displace the
			// platform, debug-symbol, or user-supplied publish arguments.
			context.Expect(command.find("-r linux-x64") != std::string::npos, "Linux profiles should publish the linux-x64 NativeAOT runtime identifier.");
			context.Expect(command.find("-p:StripSymbols=true") != std::string::npos, "Strip output should remain present with other publish options.");
			context.Expect(command.find("-p:DebugType=none") != std::string::npos, "Disabled debug symbols should suppress debug output.");
			context.Expect(command.find("-p:InvariantGlobalization=true") != std::string::npos, "Additional publish arguments should be appended after built-in options.");
			context.Expect(command.rfind("-p:RestoreSources=\"C:/Installed Editor/NativeAotNuGet/feed/\"")
				> command.rfind("https://example.invalid"), "The installed feed must override a profile-provided restore source.");
			context.Expect(command.rfind("-p:BaseIntermediateOutputPath=\"C:/Temp/TEAOT-command-test/obj/\"")
					> command.rfind("C:/Attacker/obj/")
					&& command.rfind("-p:MSBuildProjectExtensionsPath=\"C:/Temp/TEAOT-command-test/obj/\"")
						> command.rfind("C:/Attacker/extensions/")
					&& command.rfind("-p:BaseOutputPath=\"C:/Temp/TEAOT-command-test/bin/\"")
						> command.rfind("C:/Attacker/bin/")
					&& command.rfind("-p:OutputPath=\"C:/Temp/TEAOT-command-test/bin/\"")
						> command.rfind("C:/Attacker/output/"),
				"Engine-owned obj and bin properties must override malicious profile values.");
			context.Expect(command.rfind("-p:PublishDir=\"C:/Temp/TEAOT-command-test/publish/\"")
					> command.rfind("C:/Attacker/publish/")
					&& command.rfind("-o \"C:/Temp/TEAOT-command-test/publish/\"")
						> command.rfind("C:/Attacker/cli-output/"),
				"Both NativeAOT publish output controls must end at the private publish directory.");
			context.Expect(command.rfind("-p:RestorePackagesPath=\"C:/Temp/TEAOT-command-test/packages/\"")
					> command.rfind("C:/Attacker/packages/"),
				"The private package cache must override a profile-provided restore cache.");
#ifdef _WIN32
			const std::string defaultItemExcludes =
				"-p:DefaultItemExcludes=\"$([MSBuild]::Unescape('obj/**\"^%\"3Bbin/**\"^%\"3BBuild/**\"^%\"3BLibrary/**\"^%\"3BTemp/**'))\"";
#else
			const std::string defaultItemExcludes =
				"-p:DefaultItemExcludes='$([MSBuild]::Unescape("
				"'\\''obj/**%3Bbin/**%3BBuild/**%3BLibrary/**%3BTemp/**'\\''"
				"))'";
#endif
			context.Expect(command.rfind(defaultItemExcludes)
					> command.rfind("C:/Attacker/generated/**"),
				"The delayed MSBuild Unescape representation must override a profile value.");
			context.Expect(command.find("-p:DefaultItemExcludes=obj/**")
					== std::string::npos,
				"Publish must not retain the ineffective bare escaped exclusion list.");
			context.Expect(
				command.find("obj/**;bin/**;Build/**;Library/**;Temp/**")
					== std::string::npos,
				"Publish must not emit raw semicolon separators that MSBuild parses as switches.");
			context.Expect(command.find("-p:RestoreFallbackFolders=") != std::string::npos
				&& command.find("-p:RestoreAdditionalProjectSources=") != std::string::npos
				&& command.find("-p:NuGetAudit=false") != std::string::npos,
				"NativeAOT publish should disable fallback, additional, and audit sources after profile arguments.");
			context.Expect(command.rfind("-p:RestoreFallbackFolders=")
					> command.rfind("C:/Attacker/fallback/")
					&& command.rfind("-p:RestoreAdditionalProjectSources=")
						> command.rfind("https://additional.invalid/")
					&& command.rfind("-p:NuGetAudit=false")
						> command.rfind("-p:NuGetAudit=true"),
				"Offline NuGet properties must be authoritative after every profile argument.");
			context.Expect(command.find("--configfile") == std::string::npos
					&& command.find("--packages") == std::string::npos
					&& command.find("--no-http-cache") == std::string::npos
					&& command.find("--disable-parallel") == std::string::npos,
				"Publish must not receive CLI switches that belong exclusively to dotnet restore.");
			context.Expect(command.ends_with("--no-restore"),
				"The fail-closed no-restore decision must remain the final publish switch after profile arguments.");

			const std::string restore = BuildNativeAotRestoreCommand(
				profile, "/tmp/Terra Game/Assembly-CSharp.csproj", toolchain);
			context.Expect(restore.find("Installed Editor/DotNet/dotnet.exe") != std::string::npos
				&& restore.find(" restore ") != std::string::npos
				&& restore.find("-p:PublishAot=true") != std::string::npos
				&& restore.find("--configfile \"C:/Installed Editor/NativeAotNuGet/NuGet.Config\"") != std::string::npos
				&& restore.find("--packages \"C:/Temp/TEAOT-command-test/packages/\"") != std::string::npos,
				"NativeAOT restore should select the bundled SDK, installed config, and private per-run cache.");
			context.Expect(restore.find("-p:BaseIntermediateOutputPath=\"C:/Temp/TEAOT-command-test/obj/\"")
					!= std::string::npos
					&& restore.find("-p:MSBuildProjectExtensionsPath=\"C:/Temp/TEAOT-command-test/obj/\"")
						!= std::string::npos
					&& restore.find("-p:BaseOutputPath=\"C:/Temp/TEAOT-command-test/bin/\"")
						!= std::string::npos
					&& restore.find("-p:OutputPath=\"C:/Temp/TEAOT-command-test/bin/\"")
						!= std::string::npos
					&& restore.find("-p:RestorePackagesPath=\"C:/Temp/TEAOT-command-test/packages/\"")
						!= std::string::npos,
				"Restore and publish must share the exact private obj, bin, and package paths.");
			context.Expect(restore.find(defaultItemExcludes) != std::string::npos,
				"Restore must use the same MSBuild-escaped and shell-quoted exclusions as publish.");
			context.Expect(
				restore.find("obj/**;bin/**;Build/**;Library/**;Temp/**")
					== std::string::npos,
				"Restore must not emit raw semicolon separators that MSBuild parses as switches.");
			context.Expect(restore.ends_with("-p:NuGetAudit=false"),
				"Offline NuGet properties must remain the final restore boundary.");
		});

		registry.Add("Editor.BuildProfile.NativeAotCommandsQuoteShellSensitivePaths", [](TestContext& context) {
			BuildProfile profile{};
			profile.platform = GetHostExportPlatform();
			NativeAotPublishContext toolchain = MakeNativeAotTestContext();
#ifdef _WIN32
			const std::filesystem::path project =
				"C:/Projects/%TERRAENGINE_PATH_SENTINEL%/Assembly-CSharp.csproj";
			const std::filesystem::path output =
				"C:/Projects/%TERRAENGINE_PATH_SENTINEL%/Managed";
			toolchain.nugetFeed =
				"C:/Installed %TERRAENGINE_PATH_SENTINEL%/NativeAotNuGet/feed";
			toolchain.packageCache =
				"C:/Temp/%TERRAENGINE_PATH_SENTINEL%/packages";
			toolchain.intermediateOutputDirectory =
				"C:/Temp/%TERRAENGINE_PATH_SENTINEL%/obj";
			toolchain.binaryOutputDirectory =
				"C:/Temp/%TERRAENGINE_PATH_SENTINEL%/bin";
			const std::string restore =
				BuildNativeAotRestoreCommand(profile, project, toolchain);
			const std::string publish =
				BuildNativeAotPublishCommand(profile, project, output, toolchain);
			const std::string projectArgument =
				"\"C:/Projects/\"^%\"TERRAENGINE_PATH_SENTINEL\"^%\"/Assembly-CSharp.csproj\"";
			const std::string outputArgument =
				"-o \"C:/Projects/\"^%\"TERRAENGINE_PATH_SENTINEL\"^%\"/Managed/\"";
			const std::string feedProperty =
				"-p:RestoreSources=\"C:/Installed \"^%\"TERRAENGINE_PATH_SENTINEL\"^%\"/NativeAotNuGet/feed/\"";
			const std::string packageArgument =
				"--packages \"C:/Temp/\"^%\"TERRAENGINE_PATH_SENTINEL\"^%\"/packages/\"";
			context.Expect(restore.find(projectArgument) != std::string::npos
					&& publish.find(projectArgument) != std::string::npos
					&& publish.find(outputArgument) != std::string::npos
					&& restore.find(feedProperty) != std::string::npos
					&& publish.find(feedProperty) != std::string::npos
					&& restore.find(packageArgument) != std::string::npos,
				"Windows NativeAOT paths must close quotes, escape each percent outside quotes, and reopen the same argument.");
			context.Expect(restore.find("^%TERRAENGINE_PATH_SENTINEL^%")
					== std::string::npos
					&& publish.find("^%TERRAENGINE_PATH_SENTINEL^%")
						== std::string::npos,
				"Windows NativeAOT paths must not leave caret escapes inside one quoted segment.");
			context.Expect(
				std::count(restore.begin(), restore.end(), '"') % 2 == 0
					&& std::count(publish.begin(), publish.end(), '"') % 2 == 0,
				"Segmented Windows NativeAOT arguments must keep their double quotes balanced.");
#else
			const std::filesystem::path project =
				"/tmp/Terra $HOME 'Game/Assembly-CSharp.csproj";
			const std::filesystem::path output =
				"/tmp/Terra $HOME 'Game/Managed";
			toolchain.nugetFeed = "/opt/Terra $HOME 'SDK/feed";
			const std::string restore =
				BuildNativeAotRestoreCommand(profile, project, toolchain);
			const std::string publish =
				BuildNativeAotPublishCommand(profile, project, output, toolchain);
			context.Expect(restore.find("'\\''") != std::string::npos
					&& publish.find("'\\''") != std::string::npos,
				"POSIX single quotes in NativeAOT paths must use the close-escape-reopen form.");
			context.Expect(restore.find("\"/tmp/Terra $HOME") == std::string::npos
					&& publish.find("\"/tmp/Terra $HOME") == std::string::npos,
				"POSIX NativeAOT paths must not use double quotes that expand shell variables.");
#endif
		});

		registry.Add("Editor.BuildProfile.WindowsArtifactNamesRemainCompatible", [](TestContext& context) {
			// These names protect the original Windows export contract while the
			// platform helpers add Linux-specific runtime naming.
			context.ExpectEqual(std::string("win-x64"), GetExportRuntimeIdentifier(ExportPlatform::Windows), "Windows exports should keep publishing win-x64 managed payloads.");
			context.ExpectEqual(std::string(".exe"), GetExportExecutableSuffix(ExportPlatform::Windows), "Windows launcher filenames should keep the .exe suffix.");
			context.ExpectEqual(std::string(".dll"), GetExportSharedLibrarySuffix(ExportPlatform::Windows), "Windows native payloads should keep the .dll suffix.");
			context.ExpectEqual(std::string("Game.exe"), GetExportLauncherFileName("Game", ExportPlatform::Windows), "Windows launcher staging should keep the executable extension.");
			context.ExpectEqual(std::string("Assembly-CSharp.dll"), GetExportManagedRuntimeLibraryName(ExportPlatform::Windows), "Windows NativeAOT payload should keep the DLL name.");
			context.ExpectEqual(std::string("TerraEngine.dll"), GetExportEngineLibraryName(ExportPlatform::Windows), "Windows engine runtime should keep the original DLL name.");
			context.ExpectEqual(std::string("glfw3.dll"), GetExportGlfwLibraryName(ExportPlatform::Windows), "Windows GLFW runtime should keep the original DLL name.");
		});

		registry.Add("Editor.BuildProfile.RequestLocalOutputOverrideDoesNotPersist", [](TestContext& context) {
			TempProfileDirectory tempDirectory;
			const std::filesystem::path profilePath = MakeTempProfilePath();
			BuildProfile storedProfile{};
			storedProfile.name = "Stored Output Profile";
			storedProfile.id = "stored-output";
			storedProfile.outputDirectory = "Builds/StoredOutput";
			storedProfile.startupScene = "Scenes/Startup.tescene";
			storedProfile.enableManagedCompilation = false;
			context.Expect(SaveBuildProfile(storedProfile, profilePath), "The source profile should be saved before applying an invocation-local override.");

			const BuildProfile loadedProfile = LoadBuildProfile(profilePath);
			BuildProfile requestProfile = loadedProfile;
			requestProfile.outputDirectory = "D:/Automation/OneOffOutput";
			const BuildProfile reloadedProfile = LoadBuildProfile(profilePath);

			context.ExpectEqual(std::filesystem::path("D:/Automation/OneOffOutput").string(), requestProfile.outputDirectory.string(), "The request copy should use its explicit output override.");
			context.ExpectEqual(std::filesystem::path("Builds/StoredOutput").string(), loadedProfile.outputDirectory.string(), "The loaded source value should remain immutable while the request is constructed.");
			context.ExpectEqual(std::filesystem::path("Builds/StoredOutput").string(), reloadedProfile.outputDirectory.string(), "Reloading should prove the output override was never persisted to disk.");
			context.ExpectEqual(profilePath.string(), reloadedProfile.filePath.string(), "The standalone profile should remain the source of truth after a one-off export.");
		});
	}
}
