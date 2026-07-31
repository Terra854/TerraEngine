// ImplementationCheck regression coverage for the provider-neutral, headless collider cook.
// These tests exercise the real staged-file publication boundary without
// launching the editor or requiring a provider-specific physics world.
#include "../TestHarness.h"

#include "PhysicsColliderCompiler.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	using TerraEngine::AssetPipeline::CompilePhysicsCollider;
	using TerraEngine::AssetPipeline::PhysicsColliderCompileRequest;

	struct ColliderCompilerFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path()
			/ ("TerraEnginePhysicsCompilerTests_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::path recipe = root / "Box.tecollider";
		std::filesystem::path output = root / "Box.tepcoll";

		ColliderCompilerFixture()
		{
			std::filesystem::create_directories(root);
			std::ofstream(recipe, std::ios::binary) << "recipe-v1";
		}
		~ColliderCompilerFixture()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	};

	PhysicsColliderCompileRequest MakeRequest(const ColliderCompilerFixture& fixture)
	{
		PhysicsColliderCompileRequest request;
		request.recipePath = fixture.recipe;
		request.outputPath = fixture.output;
		request.settingsBytes = {std::byte{0x56}, std::byte{0x00}};
		request.callbacks.validate = [](const auto&, const auto& bytes, auto& prepared, auto&)
		{
			prepared = bytes;
			return true;
		};
		request.callbacks.cook = [](const auto& prepared, const auto& staged, const auto&, auto&)
		{
			std::ofstream stream(staged, std::ios::binary | std::ios::trunc);
			stream.write(reinterpret_cast<const char*>(prepared.data()), static_cast<std::streamsize>(prepared.size()));
			return static_cast<bool>(stream);
		};
		request.callbacks.validateStagedOutput = [](const auto& staged, auto&)
		{
			return std::filesystem::exists(staged) && std::filesystem::file_size(staged) != 0U;
		};
		return request;
	}

	std::string ReadColliderFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}

	void RegisterPhysicsColliderCompilerTests(TestRegistry& registry)
	{
		registry.Add("Physics.Editor.ColliderCompiler.AtomicPublication", [](TestContext& context) {
			ColliderCompilerFixture fixture;
			auto request = MakeRequest(fixture);
			request.callbacks.verifyPublicationInputs = [](std::uint64_t, std::uint64_t, auto&) { return true; };
			const auto report = CompilePhysicsCollider(request);
			context.Expect(report.published, "A valid staged collider must publish.");
			context.ExpectEqual("recipe-v1", ReadColliderFile(fixture.output), "Published bytes must come from the validated recipe.");
			context.Expect(!std::filesystem::exists(fixture.output.string() + ".staged"), "Successful publication must remove staging files.");
			context.ExpectEqual(std::string("e77f175595e64cb44218cc9d9d56fc365ad0e36a"),
				std::string(TerraEngine::AssetPipeline::kPhysicsColliderProviderRevision),
				"Collider compiler tests must pin the approved Jolt 5.6 provider revision.");
		});

		registry.Add("Physics.Editor.ColliderCompiler.ChangedInputsPreserveLastGood", [](TestContext& context) {
			ColliderCompilerFixture fixture;
			std::ofstream(fixture.output, std::ios::binary) << "last-known-good";
			auto request = MakeRequest(fixture);
			request.callbacks.verifyPublicationInputs = [](std::uint64_t, std::uint64_t, auto&) { return false; };
			const auto report = CompilePhysicsCollider(request);
			context.Expect(!report.published, "A changed recipe/settings fingerprint must reject publication.");
			context.Expect(report.preservedLastKnownGood, "Rejected stale output must retain the last-known-good marker.");
			context.ExpectEqual("last-known-good", ReadColliderFile(fixture.output), "Changed inputs must preserve the prior cooked output.");
			context.Expect(!std::filesystem::exists(fixture.output.string() + ".staged"), "Rejected stale output must clean staging files.");
		});

		registry.Add("Physics.Editor.ColliderCompiler.CancellationAndFailureAreAtomic", [](TestContext& context) {
			ColliderCompilerFixture fixture;
			std::ofstream(fixture.output, std::ios::binary) << "prior";
			std::atomic_bool cancelled = true;
			auto cancelledRequest = MakeRequest(fixture);
			cancelledRequest.cancellationRequested = &cancelled;
			const auto cancelledReport = CompilePhysicsCollider(cancelledRequest);
			context.Expect(cancelledReport.stage == TerraEngine::AssetPipeline::PhysicsColliderCompileStage::Cancelled, "Pre-cancelled cooks must stop before validation.");
			context.ExpectEqual("prior", ReadColliderFile(fixture.output), "Cancellation must preserve prior output.");

			cancelled.store(false);
			auto failedRequest = MakeRequest(fixture);
			failedRequest.callbacks.cook = [](const auto&, const auto& staged, const auto&, auto&) {
				std::ofstream(staged, std::ios::binary) << "partial";
				return false;
			};
			const auto failedReport = CompilePhysicsCollider(failedRequest);
			context.Expect(!failedReport.published, "A provider cook failure must not publish.");
			context.Expect(failedReport.preservedLastKnownGood, "Cook failure must preserve the existing output.");
			context.ExpectEqual("prior", ReadColliderFile(fixture.output), "Cook failure must leave the previous bytes untouched.");
			context.Expect(!std::filesystem::exists(fixture.output.string() + ".staged"), "Cook failure must remove partial staging files.");
		});

		registry.Add("Physics.Editor.ColliderCompiler.UnchangedOutputReusesCache", [](TestContext& context) {
			ColliderCompilerFixture fixture;
			auto firstRequest = MakeRequest(fixture);
			const auto first = CompilePhysicsCollider(firstRequest);
			context.Expect(first.published, "The cache fixture requires an initial atomic publication.");
			const std::string original = ReadColliderFile(fixture.output);

			bool providerCookCalled = false;
			auto secondRequest = MakeRequest(fixture);
			secondRequest.callbacks.canReusePublishedOutput = [&](
				const std::filesystem::path& output,
				const std::vector<std::byte>&,
				std::uint64_t sourceFingerprint,
				std::uint64_t settingsFingerprint,
				auto&) {
				return std::filesystem::exists(output)
					&& sourceFingerprint == first.sourceFingerprint
					&& settingsFingerprint == first.settingsFingerprint;
			};
			secondRequest.callbacks.cook = [&](const auto&, const auto&, const auto&, auto&) {
				providerCookCalled = true;
				return false;
			};
			const auto second = CompilePhysicsCollider(secondRequest);
			context.Expect(second.published && second.reusedCachedOutput,
				"An unchanged validated output must complete through the cache path.");
			context.Expect(!providerCookCalled, "Cache reuse must skip duplicate provider cooking.");
			context.ExpectEqual(original, ReadColliderFile(fixture.output),
				"Cache reuse must leave the immutable published bytes untouched.");
		});

		registry.Add("Physics.Editor.ColliderCompiler.ChangedRecipeReimports", [](TestContext& context) {
			ColliderCompilerFixture fixture;
			const auto first = CompilePhysicsCollider(MakeRequest(fixture));
			context.Expect(first.published, "The reimport fixture requires an initial publication.");
			std::ofstream(fixture.recipe, std::ios::binary | std::ios::trunc) << "recipe-v2";
			const auto second = CompilePhysicsCollider(MakeRequest(fixture));
			context.Expect(second.published && !second.reusedCachedOutput,
				"Changed recipe content must execute a fresh cook and publish a new generation.");
			context.Expect(first.sourceFingerprint != second.sourceFingerprint,
				"Reimport must record the changed source fingerprint.");
			context.ExpectEqual("recipe-v2", ReadColliderFile(fixture.output),
				"A successful changed-input reimport must replace the prior output atomically.");
		});
	}
}
