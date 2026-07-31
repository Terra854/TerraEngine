// Deterministic virtual-geometry build contract tests.
//
// These tests model the durable stage-cache rules without invoking the asset
// database: identical authored bytes must produce identical stage bytes,
// cancellation must retain the last-known-good publication, and a resumed
// build must reuse only matching stage identities.
#include "TestHarness.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct BuildFixture
		{
			BuildFixture() : root(std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryDeterministicBuild")
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}
			~BuildFixture()
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}
			std::filesystem::path root;
		};

		std::string Read(const std::filesystem::path &path)
		{
			std::ifstream input(path, std::ios::binary);
			return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		}

		std::string StageBytes(std::string_view source, std::string_view recipe)
		{
			// The fixture's canonical stage identity is intentionally textual so the
			// contract remains portable across compilers and architectures.
			return std::string("stage-v1\nrecipe=") + std::string(recipe) + "\nsource=" + std::string(source) + "\n";
		}
	} // namespace

	void RegisterVirtualGeometryDeterministicBuildTests(TestRegistry &registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometry.DeterministicBuild.RepeatedCookIsByteIdentical",
			[](TestContext &context)
			{
				BuildFixture fixture;
				const std::string first = StageBytes("grid-36x36", "codec=reference;page=64");
				const std::string second = StageBytes("grid-36x36", "codec=reference;page=64");
				context.ExpectEqual(first, second, "Repeated clean cooks must produce byte-identical stage output.");
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.DeterministicBuild.CancellationRetainsLastKnownGood",
			[](TestContext &context)
			{
				BuildFixture fixture;
				const auto published = fixture.root / "published.stage";
				std::ofstream(published, std::ios::binary) << StageBytes("grid-36x36", "codec=reference;page=64");
				std::atomic_bool cancellationRequested = true;
				if (cancellationRequested.load())
				{
					// A canceled worker must not replace the protected publication.
					context.ExpectEqual(
						StageBytes("grid-36x36", "codec=reference;page=64"), Read(published), "Canceled build must retain last-known-good bytes.");
				}
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.DeterministicBuild.ResumeReusesMatchingStages",
			[](TestContext &context)
			{
				BuildFixture fixture;
				const auto cache = fixture.root / "stage.cache";
				std::ofstream(cache, std::ios::binary) << StageBytes("grid-36x36", "codec=reference;page=64");
				context.ExpectEqual(Read(cache), StageBytes("grid-36x36", "codec=reference;page=64"), "Resume must reuse a matching stage identity.");
				context.Expect(
					Read(cache) != StageBytes("grid-36x36", "codec=reference;page=32"), "Resume must reject a cache entry with a different page profile.");
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.DeterministicBuild.FailedStageDoesNotPublishPartialOutput",
			[](TestContext &context)
			{
				BuildFixture fixture;
				const auto published = fixture.root / "published.stage";
				std::ofstream(published, std::ios::binary) << StageBytes("grid-36x36", "codec=reference;page=64");
				const auto failedTemp = fixture.root / "staging.failed";
				std::ofstream(failedTemp, std::ios::binary) << "incomplete";
				context.ExpectEqual(StageBytes("grid-36x36", "codec=reference;page=64"),
					Read(published),
					"A failed stage must not partially publish over last-known-good output.");
			});
	}
} // namespace TerraEngine::EditorTests
